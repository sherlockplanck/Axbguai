#include "mcu_uart.h"
#include "application.h"

#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "McuUart"

namespace {
// 串口功能说明：
// 1. ESP32-S3 TX 接另一块单片机 RX，ESP32-S3 RX 接另一块单片机 TX，两块板必须共地。
// 2. 当前使用 UART1，默认 TX=GPIO17、RX=GPIO8、波特率=115200。
// 3. 如果要换引脚或波特率，只改下面 kMcuUartTxPin、kMcuUartRxPin、kMcuUartBaudRate。
// 4. 避免使用已经被音频、屏幕、按键、LED 占用的 GPIO。当前 bread-compact-wifi 板型中 GPIO4/5/6/7/15/16 已被 I2S 音频占用。
constexpr uart_port_t kMcuUartPort = UART_NUM_1;
constexpr gpio_num_t kMcuUartTxPin = GPIO_NUM_17;
constexpr gpio_num_t kMcuUartRxPin = GPIO_NUM_8;
constexpr int kMcuUartBaudRate = 115200;

// 接收/发送缓冲区大小。普通命令字符串保持 2048 足够；如果你要传较大的二进制包，可适当调大。
constexpr int kMcuUartRxBufferSize = 2048;
constexpr int kMcuUartTxBufferSize = 2048;

// 串口接收任务配置。任务优先级不要随意调太高，避免影响音频处理。
constexpr int kMcuUartRxTaskStackSize = 4096;
constexpr UBaseType_t kMcuUartRxTaskPriority = 5;
constexpr size_t kMcuUartMaxLineLength = 256;
constexpr int kMcuUartReadTimeoutMs = 100;
constexpr size_t kMcuUartMaxTextMsgBytes = 128;
constexpr int kMcuUartAckTimeoutMs = 100;
constexpr int kMcuUartMinTxIntervalMs = 100;
constexpr int kMcuUartAckMaxRetries = 3;
constexpr char kAckCmdCall[] = "call";
constexpr char kAckCmdContactUpdate[] = "contact_update";

struct PendingAckCommand {
    std::string ack_cmd;
    std::string line;
    TickType_t last_send_tick = 0;
    int retry_count = 0;
    bool active = false;
};

std::mutex g_stm32_data_mutex;
McuUartStm32Data g_latest_stm32_data;
std::string g_uart_line_buffer;
std::mutex g_pending_ack_mutex;
PendingAckCommand g_pending_call_ack;
PendingAckCommand g_pending_contact_update_ack;
std::mutex g_uart_tx_mutex;
TickType_t g_last_uart_tx_tick = 0;

std::string TrimString(const std::string& input) {
    size_t start = input.find_first_not_of(" \r\n\t");
    if (start == std::string::npos) {
        return "";
    }

    size_t end = input.find_last_not_of(" \r\n\t");
    return input.substr(start, end - start + 1);
}

bool GetFieldValue(const std::string& line, const char* field_name, std::string& value) {
    std::string marker = std::string(field_name) + "=";
    size_t start = line.find(marker);
    if (start == std::string::npos) {
        return false;
    }

    start += marker.size();
    size_t end = line.find(',', start);
    value = TrimString(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
    return !value.empty();
}

bool ParseDoubleText(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }

    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    return *end == '\0';
}

bool ParseBoolText(const std::string& text, bool& value) {
    if (text == "1" || text == "true" || text == "TRUE") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE") {
        value = false;
        return true;
    }
    return false;
}

esp_err_t WriteBytesToMcuUart(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(g_uart_tx_mutex);
    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = pdMS_TO_TICKS(kMcuUartMinTxIntervalMs);
    if (g_last_uart_tx_tick != 0) {
        TickType_t elapsed = now - g_last_uart_tx_tick;
        if (elapsed < min_interval) {
            vTaskDelay(min_interval - elapsed);
        }
    }

    int written = uart_write_bytes(kMcuUartPort, data, len);
    g_last_uart_tx_tick = xTaskGetTickCount();
    return written == static_cast<int>(len) ? ESP_OK : ESP_FAIL;
}

esp_err_t WriteTextToMcuUart(const char* text) {
    if (text == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    return WriteBytesToMcuUart(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

PendingAckCommand* PendingAckSlot(const std::string& ack_cmd) {
    if (ack_cmd == kAckCmdCall) {
        return &g_pending_call_ack;
    }
    if (ack_cmd == kAckCmdContactUpdate) {
        return &g_pending_contact_update_ack;
    }
    return nullptr;
}

void TrackAckCommand(const char* ack_cmd, const std::string& line) {
    if (ack_cmd == nullptr || line.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_ack_mutex);
    PendingAckCommand* pending = PendingAckSlot(ack_cmd);
    if (pending == nullptr) {
        return;
    }

    pending->ack_cmd = ack_cmd;
    pending->line = line;
    pending->last_send_tick = xTaskGetTickCount();
    pending->retry_count = 0;
    pending->active = true;
    ESP_LOGI(TAG, "Waiting STM32 ACK: cmd=%s", ack_cmd);
}

bool HandleAckLine(const std::string& line) {
    constexpr char kPrefix[] = "ESP32_ACK,";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    std::string ack_cmd;
    std::string received_text;
    bool received = false;
    if (!GetFieldValue(line, "cmd", ack_cmd)
        || !GetFieldValue(line, "received", received_text)
        || !ParseBoolText(received_text, received)) {
        ESP_LOGW(TAG, "Invalid STM32 ACK: %s", line.c_str());
        return true;
    }

    if (!received) {
        ESP_LOGW(TAG, "STM32 ACK received=0: cmd=%s", ack_cmd.c_str());
        return true;
    }

    std::lock_guard<std::mutex> lock(g_pending_ack_mutex);
    PendingAckCommand* pending = PendingAckSlot(ack_cmd);
    if (pending != nullptr && pending->active) {
        pending->active = false;
        ESP_LOGI(TAG, "STM32 ACK matched: cmd=%s", ack_cmd.c_str());
        Application::GetInstance().OnMcuAckReceived(ack_cmd);
    } else {
        ESP_LOGI(TAG, "STM32 ACK without pending command: cmd=%s", ack_cmd.c_str());
        Application::GetInstance().OnMcuAckReceived(ack_cmd);
    }
    return true;
}

void RetryPendingAckCommand(const char* ack_cmd) {
    std::string line;
    std::string pending_cmd;
    int retry_count = 0;
    bool give_up = false;
    TickType_t now = xTaskGetTickCount();

    {
        std::lock_guard<std::mutex> lock(g_pending_ack_mutex);
        PendingAckCommand* pending = PendingAckSlot(ack_cmd);
        if (pending == nullptr || !pending->active) {
            return;
        }
        if ((now - pending->last_send_tick) < pdMS_TO_TICKS(kMcuUartAckTimeoutMs)) {
            return;
        }

        pending_cmd = pending->ack_cmd;
        if (pending->retry_count >= kMcuUartAckMaxRetries) {
            pending->active = false;
            give_up = true;
        } else {
            pending->retry_count++;
            pending->last_send_tick = now;
            retry_count = pending->retry_count;
            line = pending->line;
        }
    }

    if (give_up) {
        ESP_LOGW(TAG, "STM32 ACK timeout: cmd=%s after %d retries",
            pending_cmd.c_str(), kMcuUartAckMaxRetries);
        return;
    }

    esp_err_t ret = WriteTextToMcuUart(line.c_str());
    ESP_LOGW(TAG, "Retry STM32 command cmd=%s retry=%d/%d result=%s",
        pending_cmd.c_str(), retry_count, kMcuUartAckMaxRetries, esp_err_to_name(ret));
}

void RetryPendingAckCommands() {
    RetryPendingAckCommand(kAckCmdCall);
    RetryPendingAckCommand(kAckCmdContactUpdate);
}

bool LooksLikeCompleteStm32DataLine(const std::string& line) {
    constexpr char kPrefix[] = "STM32_DATA,";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    std::string gps_signal_text;
    std::string fall_alarm_text;
    bool gps_signal = false;
    bool fall_alarm = false;
    if (!GetFieldValue(line, "gps_signal", gps_signal_text)
        || !GetFieldValue(line, "fall_alarm", fall_alarm_text)
        || !ParseBoolText(gps_signal_text, gps_signal)
        || !ParseBoolText(fall_alarm_text, fall_alarm)) {
        return false;
    }

    if (!gps_signal) {
        return true;
    }

    return line.find("longitude=") != std::string::npos
        && line.find("latitude=") != std::string::npos;
}

bool ParseStm32DataLine(const std::string& line, McuUartStm32Data& parsed_data) {
    constexpr char kPrefix[] = "STM32_DATA,";
    if (line.rfind(kPrefix, 0) != 0) {
        return false;
    }

    std::string gps_signal_text;
    std::string longitude_text;
    std::string latitude_text;
    std::string fall_alarm_text;
    std::string gps_status_text;
    if (!GetFieldValue(line, "gps_signal", gps_signal_text)
        || !GetFieldValue(line, "fall_alarm", fall_alarm_text)) {
        ESP_LOGW(TAG, "STM32_DATA missing required field: %s", line.c_str());
        return false;
    }

    bool gps_signal = false;
    bool fall_alarm = false;
    if (!ParseBoolText(gps_signal_text, gps_signal)
        || !ParseBoolText(fall_alarm_text, fall_alarm)) {
        ESP_LOGW(TAG, "STM32_DATA field parse failed: %s", line.c_str());
        return false;
    }

    parsed_data.gps_signal = gps_signal;
    parsed_data.fall_alarm = fall_alarm;
    parsed_data.valid = true;

    if (!gps_signal) {
        if (GetFieldValue(line, "gps_status", gps_status_text)) {
            parsed_data.gps_status = gps_status_text;
        } else {
            parsed_data.gps_status = "no_signal";
        }
        return true;
    }

    if (!GetFieldValue(line, "longitude", longitude_text)
        || !GetFieldValue(line, "latitude", latitude_text)) {
        ESP_LOGW(TAG, "STM32_DATA missing GPS coordinate field: %s", line.c_str());
        return false;
    }

    double longitude = 0.0;
    double latitude = 0.0;
    if (!ParseDoubleText(longitude_text, longitude)
        || !ParseDoubleText(latitude_text, latitude)) {
        ESP_LOGW(TAG, "STM32_DATA field parse failed: %s", line.c_str());
        return false;
    }

    parsed_data.longitude = longitude;
    parsed_data.latitude = latitude;
    parsed_data.gps_status.clear();
    return true;
}

void SaveStm32Data(const McuUartStm32Data& parsed_data) {
    std::lock_guard<std::mutex> lock(g_stm32_data_mutex);
    uint32_t next_update_count = g_latest_stm32_data.update_count + 1;
    g_latest_stm32_data = parsed_data;
    g_latest_stm32_data.update_count = next_update_count;
}

void HandleMcuUartLine(const std::string& raw_line) {
    std::string line = TrimString(raw_line);
    if (line.empty()) {
        return;
    }

    if (HandleAckLine(line)) {
        return;
    }

    McuUartStm32Data parsed_data;
    if (!ParseStm32DataLine(line, parsed_data)) {
        ESP_LOGW(TAG, "Unknown UART line: %s", line.c_str());
        return;
    }

    SaveStm32Data(parsed_data);
    if (parsed_data.gps_signal) {
        ESP_LOGI(TAG, "STM32_DATA gps_signal=1 longitude=%.8f latitude=%.8f fall_alarm=%d",
            parsed_data.longitude, parsed_data.latitude, parsed_data.fall_alarm ? 1 : 0);
    } else {
        ESP_LOGI(TAG, "STM32_DATA gps_signal=0 gps_status=%s fall_alarm=%d",
            parsed_data.gps_status.c_str(), parsed_data.fall_alarm ? 1 : 0);
    }
}

void HandleMcuUartReceivedData(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        if (!g_uart_line_buffer.empty()) {
            ESP_LOGI(TAG, "UART idle, parse buffered line without newline");
            HandleMcuUartLine(g_uart_line_buffer);
            g_uart_line_buffer.clear();
        }
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        char ch = static_cast<char>(data[i]);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            HandleMcuUartLine(g_uart_line_buffer);
            g_uart_line_buffer.clear();
            continue;
        }

        if (g_uart_line_buffer.size() >= kMcuUartMaxLineLength) {
            ESP_LOGW(TAG, "UART line too long, drop buffer. Check whether sender appends \\r\\n");
            g_uart_line_buffer.clear();
            continue;
        }
        g_uart_line_buffer.push_back(ch);

        if (LooksLikeCompleteStm32DataLine(g_uart_line_buffer)) {
            HandleMcuUartLine(g_uart_line_buffer);
            g_uart_line_buffer.clear();
        }
    }
}

void McuUartRxTask(void* arg) {
    uint8_t data[256];

    while (true) {
        // 阻塞等待串口数据。推荐 STM32 每条消息以 \r\n 结尾；如果没有换行，
        // 这里会在串口短暂空闲后尝试按一条完整文本解析。
        int len = uart_read_bytes(kMcuUartPort, data, sizeof(data), pdMS_TO_TICKS(kMcuUartReadTimeoutMs));
        if (len > 0) {
            HandleMcuUartReceivedData(data, static_cast<size_t>(len));
        } else {
            HandleMcuUartReceivedData(nullptr, 0);
        }
        RetryPendingAckCommands();
    }
}
} // namespace

esp_err_t mcu_uart_init() {
    // 串口初始化函数，在 app_main() 中调用一次。
    uart_config_t uart_config = {};
    uart_config.baud_rate = kMcuUartBaudRate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t ret = uart_driver_install(
        kMcuUartPort,
        kMcuUartRxBufferSize,
        kMcuUartTxBufferSize,
        0,
        nullptr,
        0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置波特率、数据位、停止位、校验位等串口参数。
    ret = uart_param_config(kMcuUartPort, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 绑定 TX/RX 引脚。如果换线，优先改文件顶部的 kMcuUartTxPin / kMcuUartRxPin。
    ret = uart_set_pin(kMcuUartPort, kMcuUartTxPin, kMcuUartRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 创建后台接收任务，持续读取另一块单片机发来的数据。
    BaseType_t task_created = xTaskCreate(
        McuUartRxTask,
        "mcu_uart_rx",
        kMcuUartRxTaskStackSize,
        nullptr,
        kMcuUartRxTaskPriority,
        nullptr);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART%d initialized: TX=%d RX=%d baud=%d",
        kMcuUartPort, kMcuUartTxPin, kMcuUartRxPin, kMcuUartBaudRate);
    return ESP_OK;
}

esp_err_t mcu_uart_send_bytes(const uint8_t* data, size_t len) {
    // 发送原始字节数据。适合发送二进制协议，例如帧头 + 长度 + 数据 + 校验。
    if (data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return WriteBytesToMcuUart(data, len);
}

esp_err_t mcu_uart_send_text(const char* text) {
    // 发送字符串。适合发送类似 "CMD:START\r\n" 这种文本命令。
    if (text == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return mcu_uart_send_bytes(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

bool mcu_uart_get_latest_stm32_data(McuUartStm32Data* data) {
    if (data == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_stm32_data_mutex);
    if (!g_latest_stm32_data.valid) {
        return false;
    }

    *data = g_latest_stm32_data;
    return true;
}

std::string SanitizeUartTextField(const char* text, const char* field_name) {
    if (text == nullptr) {
        return "";
    }

    std::string value(text);
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        } else if (ch == ',') {
            ch = ';';
        }
    }
    if (value.size() > kMcuUartMaxTextMsgBytes) {
        ESP_LOGW(TAG, "%s too long, truncate to %u bytes",
            field_name == nullptr ? "uart field" : field_name,
            static_cast<unsigned>(kMcuUartMaxTextMsgBytes));
        value.resize(kMcuUartMaxTextMsgBytes);
    }
    return value;
}

std::string SanitizeTextMessage(const char* text_msg) {
    return SanitizeUartTextField(text_msg, "text_msg");
}

esp_err_t mcu_uart_send_call_cmd(int call_cmd) {
    if (call_cmd != 1 && call_cmd != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    std::string line = "ESP32_CMD,call_cmd=";
    line += call_cmd == 2 ? "2" : "1";
    line += "\r\n";

    esp_err_t ret = mcu_uart_send_text(line.c_str());
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TX %s", line.c_str());
        TrackAckCommand(kAckCmdCall, line);
    } else {
        ESP_LOGW(TAG, "Failed to send call command");
    }
    return ret;
}

esp_err_t mcu_uart_send_esp32_cmd(bool call_cmd, const char* text_msg) {
    std::string line = "ESP32_CMD,call_cmd=";
    line += call_cmd ? "1" : "0";
    line += ",text_msg=";
    line += SanitizeTextMessage(text_msg);
    line += "\r\n";

    esp_err_t ret = mcu_uart_send_text(line.c_str());
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TX %s", line.c_str());
        if (call_cmd) {
            TrackAckCommand(kAckCmdCall, line);
        }
    } else {
        ESP_LOGW(TAG, "Failed to send ESP32_CMD");
    }
    return ret;
}

esp_err_t mcu_uart_send_contact_update(
    const char* contact1,
    bool contact1_priority,
    const char* contact2,
    bool contact2_priority) {
    std::string line = "ESP32_CMD,contact1=";
    line += SanitizeUartTextField(contact1, "contact1");
    line += ",contact1_priority=";
    line += contact1_priority ? "1" : "0";
    line += ",contact2=";
    line += SanitizeUartTextField(contact2, "contact2");
    line += ",contact2_priority=";
    line += contact2_priority ? "1" : "0";
    line += "\r\n";

    esp_err_t ret = mcu_uart_send_text(line.c_str());
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TX %s", line.c_str());
        TrackAckCommand(kAckCmdContactUpdate, line);
    } else {
        ESP_LOGW(TAG, "Failed to send contact update");
    }
    return ret;
}

extern "C" void send_cmd_to_mcu(const char* data) {
    // 兼容 application.cc 中已有的语音命令下发逻辑。
    // 例如识别到“发送信息”后，会通过这里向另一块单片机发送 "CMD:START\r\n"。
    if (mcu_uart_send_text(data) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send command to MCU");
    }
}
