#ifndef MCU_UART_H
#define MCU_UART_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <esp_err.h>

struct McuUartStm32Data {
    bool gps_signal = false;
    double longitude = 0.0;
    double latitude = 0.0;
    bool fall_alarm = false;
    std::string gps_status;
    bool valid = false;
    uint32_t update_count = 0;
};

// 初始化与另一块单片机通信的 UART。
// 当前具体端口、TX/RX 引脚、波特率在 mcu_uart.cc 顶部修改。
esp_err_t mcu_uart_init();

// 发送原始字节数据，适合二进制协议。
esp_err_t mcu_uart_send_bytes(const uint8_t* data, size_t len);

// 发送以 '\0' 结尾的字符串，适合文本命令。
esp_err_t mcu_uart_send_text(const char* text);

// 获取最近一次解析成功的 STM32 数据。尚未收到有效数据时返回 false。
bool mcu_uart_get_latest_stm32_data(McuUartStm32Data* data);

// 按文本协议发送给 STM32：ESP32_CMD,call_cmd=0/1,text_msg=...\r\n
esp_err_t mcu_uart_send_esp32_cmd(bool call_cmd, const char* text_msg);

// Send call command to STM32: ESP32_CMD,call_cmd=1/2\r\n
esp_err_t mcu_uart_send_call_cmd(int call_cmd);

// Send contact update to STM32:
// ESP32_CMD,contact1=...,contact1_priority=0/1,contact2=...,contact2_priority=0/1\r\n
esp_err_t mcu_uart_send_contact_update(
    const char* contact1,
    bool contact1_priority,
    const char* contact2,
    bool contact2_priority);

// 给 application.cc 里的语音命令调用使用。
// 如果后续不需要兼容旧调用，可以直接改用 mcu_uart_send_text()。
extern "C" void send_cmd_to_mcu(const char* data);

#endif // MCU_UART_H
