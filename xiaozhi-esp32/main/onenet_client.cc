#include "onenet_client.h"

#include "application.h"
#include "board.h"
#include "mcu_uart.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#define TAG "OneNET"

namespace {
// OneNET MQTT 接入配置。
// 如果在 OneNET 控制台重新生成了 token，只需要改这里的 kOnenetToken。
constexpr char kOnenetBrokerHost[] = "mqtts.heclouds.com";
constexpr int kOnenetBrokerPort = 1883;
constexpr int kOnenetMqttConnectId = 1;
constexpr char kOnenetProductId[] = "C21bW84s72";
constexpr char kOnenetDeviceName[] = "data";
constexpr char kOnenetToken[] = "version=2018-10-31&res=products%2FC21bW84s72%2Fdevices%2Fdata&et=1910080768&method=md5&sign=1HjjPj0U%2B0xJoTLkBSn1hw%3D%3D";

// OneNET 数据流/命令字段名，必须和云平台产品物模型中的标识符一致。
constexpr char kFallAlarmProperty[] = "fall_alarm";
constexpr char kLatitudeProperty[] = "latitude";
constexpr char kLongitudeProperty[] = "longitude";
constexpr char kCallCmdProperty[] = "call_cmd";
constexpr char kTextMsgProperty[] = "text_msg";
constexpr char kGuaiMessageProperty[] = "guai_message";
constexpr char kContact1Property[] = "contact1";
constexpr char kContact2Property[] = "contact2";
constexpr char kContact1PriorityProperty[] = "contact1_priority";
constexpr char kContact2PriorityProperty[] = "contact2_priority";
constexpr char kName1Property[] = "name1";
constexpr char kName2Property[] = "name2";

constexpr int kOnenetTaskStackSize = 8192;
constexpr UBaseType_t kOnenetTaskPriority = 4;
constexpr int kOnenetReconnectDelayMs = 5000;
constexpr int kOnenetLoopDelayMs = 1000;
constexpr int kOnenetKeepAliveSeconds = 30;
constexpr int kOnenetPeriodicUploadMs = 10000;
constexpr double kCoordinateChangeThreshold = 0.000001;

std::unique_ptr<Mqtt> g_mqtt;
std::mutex g_mqtt_mutex;
std::atomic_bool g_task_started(false);
std::atomic_bool g_mqtt_connected(false);

struct ContactUpdateCache {
    std::string contact1;
    std::string contact2;
    bool contact1_priority = false;
    bool contact2_priority = false;
    bool has_contact1 = false;
    bool has_contact2 = false;
    bool has_contact1_priority = false;
    bool has_contact2_priority = false;
};

std::mutex g_contact_update_mutex;
ContactUpdateCache g_contact_update_cache;

std::string PropertyPostTopic() {
    return std::string("$sys/") + kOnenetProductId + "/" + kOnenetDeviceName + "/thing/property/post";
}

std::string PropertyPostReplyTopic() {
    return PropertyPostTopic() + "/reply";
}

std::string PropertySetTopic() {
    return std::string("$sys/") + kOnenetProductId + "/" + kOnenetDeviceName + "/thing/property/set";
}

std::string PropertySetReplyTopic() {
    return std::string("$sys/") + kOnenetProductId + "/" + kOnenetDeviceName + "/thing/property/set_reply";
}

std::string NextMessageId() {
    static uint32_t id = 0;
    return std::to_string(++id);
}

std::string PrintJson(cJSON* root) {
    char* json = cJSON_PrintUnformatted(root);
    if (json == nullptr) {
        return "";
    }

    std::string result(json);
    cJSON_free(json);
    return result;
}

void AddDataPointValue(cJSON* dp, const char* name, double value) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "value", value);
    cJSON_AddItemToObject(dp, name, item);
}

void AddDataPointValue(cJSON* dp, const char* name, bool value) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "value", value);
    cJSON_AddItemToObject(dp, name, item);
}

void AddDataPointValue(cJSON* dp, const char* name, const std::string& value) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "value", value.c_str());
    cJSON_AddItemToObject(dp, name, item);
}

bool IsValidCoordinate(double value, double min_value, double max_value) {
    return value >= min_value && value <= max_value;
}

double RoundCoordinate(double value) {
    return std::round(value * 1000000.0) / 1000000.0;
}

const cJSON* ExtractPropertyValue(const cJSON* item) {
    if (cJSON_IsObject(item)) {
        const cJSON* value = cJSON_GetObjectItem(item, "value");
        if (value != nullptr) {
            return value;
        }
    }
    return item;
}

bool JsonToBool(const cJSON* item, bool& value) {
    item = ExtractPropertyValue(item);
    if (cJSON_IsBool(item)) {
        value = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        value = item->valuedouble != 0;
        return true;
    }
    if (cJSON_IsString(item)) {
        std::string text(item->valuestring);
        if (text == "1" || text == "true" || text == "TRUE") {
            value = true;
            return true;
        }
        if (text == "0" || text == "false" || text == "FALSE") {
            value = false;
            return true;
        }
    }
    return false;
}

std::string JsonToText(const cJSON* item) {
    item = ExtractPropertyValue(item);
    if (cJSON_IsString(item)) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f", item->valuedouble);
        return buffer;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? "1" : "0";
    }
    return "";
}

void PublishSetReply(const std::string& id, int code, const char* message) {
    cJSON* root = cJSON_CreateObject();
    std::string reply_id = id.empty() ? NextMessageId() : id;
    cJSON_AddStringToObject(root, "id", reply_id.c_str());
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", message);

    std::string payload = PrintJson(root);
    cJSON_Delete(root);
    if (payload.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mqtt_mutex);
    if (g_mqtt && g_mqtt_connected.load()) {
        g_mqtt->Publish(PropertySetReplyTopic(), payload);
    }
}

bool ParseKeyValueCommand(const std::string& payload, bool& call_cmd, std::string& text_msg) {
    size_t call_pos = payload.find("call_cmd=");
    if (call_pos == std::string::npos) {
        return false;
    }

    call_pos += strlen("call_cmd=");
    size_t call_end = payload.find(',', call_pos);
    std::string call_value = payload.substr(call_pos, call_end == std::string::npos ? std::string::npos : call_end - call_pos);
    if (call_value == "1" || call_value == "true" || call_value == "TRUE") {
        call_cmd = true;
    } else if (call_value == "0" || call_value == "false" || call_value == "FALSE") {
        call_cmd = false;
    } else {
        return false;
    }

    size_t text_pos = payload.find("text_msg=");
    if (text_pos != std::string::npos) {
        text_msg = payload.substr(text_pos + strlen("text_msg="));
    }
    return true;
}

bool UpdateContactCache(
    const cJSON* params,
    bool& ready,
    std::string& contact1,
    bool& contact1_priority,
    std::string& contact2,
    bool& contact2_priority) {
    const cJSON* contact1_item = cJSON_GetObjectItem(params, kContact1Property);
    const cJSON* contact2_item = cJSON_GetObjectItem(params, kContact2Property);
    const cJSON* contact1_priority_item = cJSON_GetObjectItem(params, kContact1PriorityProperty);
    const cJSON* contact2_priority_item = cJSON_GetObjectItem(params, kContact2PriorityProperty);

    std::lock_guard<std::mutex> lock(g_contact_update_mutex);
    if (contact1_item != nullptr) {
        g_contact_update_cache.contact1 = JsonToText(contact1_item);
        g_contact_update_cache.has_contact1 = true;
    }
    if (contact2_item != nullptr) {
        g_contact_update_cache.contact2 = JsonToText(contact2_item);
        g_contact_update_cache.has_contact2 = true;
    }
    if (contact1_priority_item != nullptr) {
        bool value = false;
        if (!JsonToBool(contact1_priority_item, value)) {
            return false;
        }
        g_contact_update_cache.contact1_priority = value;
        g_contact_update_cache.has_contact1_priority = true;
    }
    if (contact2_priority_item != nullptr) {
        bool value = false;
        if (!JsonToBool(contact2_priority_item, value)) {
            return false;
        }
        g_contact_update_cache.contact2_priority = value;
        g_contact_update_cache.has_contact2_priority = true;
    }

    ready = g_contact_update_cache.has_contact1
        && g_contact_update_cache.has_contact2
        && g_contact_update_cache.has_contact1_priority
        && g_contact_update_cache.has_contact2_priority;
    if (ready) {
        contact1 = g_contact_update_cache.contact1;
        contact1_priority = g_contact_update_cache.contact1_priority;
        contact2 = g_contact_update_cache.contact2;
        contact2_priority = g_contact_update_cache.contact2_priority;
    }
    return true;
}

void HandlePropertySetPayload(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        bool call_cmd = false;
        std::string text_msg;
        if (ParseKeyValueCommand(payload, call_cmd, text_msg)) {
            esp_err_t ret = mcu_uart_send_esp32_cmd(call_cmd, text_msg.c_str());
            if (!text_msg.empty()) {
                Application::GetInstance().OnFamilyMessageReceived(text_msg);
            }
            PublishSetReply("", ret == ESP_OK ? 200 : 500, ret == ESP_OK ? "success" : "uart send failed");
            return;
        }

        ESP_LOGW(TAG, "Invalid OneNET property set payload: %s", payload.c_str());
        PublishSetReply("", 400, "invalid property set payload");
        return;
    }

    std::string id = JsonToText(cJSON_GetObjectItem(root, "id"));
    const cJSON* params = cJSON_GetObjectItem(root, "params");
    if (!cJSON_IsObject(params)) {
        params = root;
    }

    const cJSON* contact1_item = cJSON_GetObjectItem(params, kContact1Property);
    const cJSON* contact2_item = cJSON_GetObjectItem(params, kContact2Property);
    const cJSON* contact1_priority_item = cJSON_GetObjectItem(params, kContact1PriorityProperty);
    const cJSON* contact2_priority_item = cJSON_GetObjectItem(params, kContact2PriorityProperty);
    bool has_contact_field = contact1_item != nullptr
        || contact2_item != nullptr
        || contact1_priority_item != nullptr
        || contact2_priority_item != nullptr;

    const cJSON* name1_item = cJSON_GetObjectItem(params, kName1Property);
    const cJSON* name2_item = cJSON_GetObjectItem(params, kName2Property);
    bool has_name_field = name1_item != nullptr || name2_item != nullptr;

    bool handled_any = false;
    bool uart_ok = true;

    if (has_name_field) {
        std::string name1 = JsonToText(name1_item);
        std::string name2 = JsonToText(name2_item);
        Application::GetInstance().OnCallContactNamesReceived(
            name1,
            name1_item != nullptr,
            name2,
            name2_item != nullptr);
        handled_any = true;
        ESP_LOGI(TAG, "Cloud call names updated: name1=%s name2=%s",
            name1_item != nullptr ? name1.c_str() : "<unchanged>",
            name2_item != nullptr ? name2.c_str() : "<unchanged>");
    }

    if (has_contact_field) {
        bool contact1_priority = false;
        bool contact2_priority = false;
        std::string contact1;
        std::string contact2;
        bool contact_ready = false;
        if (!UpdateContactCache(
                params,
                contact_ready,
                contact1,
                contact1_priority,
                contact2,
                contact2_priority)) {
            cJSON_Delete(root);
            PublishSetReply(id, 400, "invalid contact priority");
            return;
        }

        handled_any = true;
        if (contact_ready) {
            esp_err_t ret = mcu_uart_send_contact_update(
                contact1.c_str(),
                contact1_priority,
                contact2.c_str(),
                contact2_priority);
            uart_ok = uart_ok && (ret == ESP_OK);
            ESP_LOGI(TAG, "Cloud contact update forwarded to STM32: contact1=%s contact1_priority=%d contact2=%s contact2_priority=%d",
                contact1.c_str(), contact1_priority ? 1 : 0, contact2.c_str(), contact2_priority ? 1 : 0);
        } else {
            ESP_LOGI(TAG, "Cloud contact field cached, waiting for remaining contact fields");
        }
    }

    bool call_cmd = false;
    bool has_call_cmd = JsonToBool(cJSON_GetObjectItem(params, kCallCmdProperty), call_cmd);
    std::string text_msg = JsonToText(cJSON_GetObjectItem(params, kTextMsgProperty));
    if (has_call_cmd || !text_msg.empty()) {
        esp_err_t ret = mcu_uart_send_esp32_cmd(call_cmd, text_msg.c_str());
        if (!text_msg.empty()) {
            Application::GetInstance().OnFamilyMessageReceived(text_msg);
        }
        uart_ok = uart_ok && (ret == ESP_OK);
        handled_any = true;
        ESP_LOGI(TAG, "Cloud command forwarded to STM32: call_cmd=%d text_msg=%s",
            call_cmd ? 1 : 0, text_msg.c_str());
    }

    if (!handled_any) {
        cJSON_Delete(root);
        PublishSetReply(id, 400, "missing command property");
        return;
    }

    PublishSetReply(id, uart_ok ? 200 : 500, uart_ok ? "success" : "uart send failed");
    cJSON_Delete(root);
}

void HandleMqttMessage(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "RX topic=%s payload=%s", topic.c_str(), payload.c_str());
    if (topic == PropertyPostReplyTopic()) {
        ESP_LOGI(TAG, "OneNET property post reply: %s", payload.c_str());
        return;
    }

    if (topic == PropertySetTopic()) {
        HandlePropertySetPayload(payload);
    }
}

bool ConnectOnenet() {
    auto mqtt = Board::GetInstance().GetNetwork()->CreateMqtt(kOnenetMqttConnectId);
    mqtt->SetKeepAlive(kOnenetKeepAliveSeconds);
    mqtt->OnConnected([]() {
        g_mqtt_connected.store(true);
        ESP_LOGI(TAG, "OneNET MQTT connected");
    });
    mqtt->OnDisconnected([]() {
        g_mqtt_connected.store(false);
        ESP_LOGW(TAG, "OneNET MQTT disconnected");
    });
    mqtt->OnMessage(HandleMqttMessage);
    mqtt->OnError([](const std::string& error) {
        g_mqtt_connected.store(false);
        ESP_LOGW(TAG, "OneNET MQTT error: %s", error.c_str());
    });

    ESP_LOGI(TAG, "Connecting to OneNET %s:%d", kOnenetBrokerHost, kOnenetBrokerPort);
    if (!mqtt->Connect(kOnenetBrokerHost, kOnenetBrokerPort, kOnenetDeviceName, kOnenetProductId, kOnenetToken)) {
        g_mqtt_connected.store(false);
        ESP_LOGW(TAG, "Connect OneNET failed, code=%d", mqtt->GetLastError());
        return false;
    }

    if (!mqtt->Subscribe(PropertySetTopic())) {
        g_mqtt_connected.store(false);
        ESP_LOGW(TAG, "Subscribe OneNET property set topic failed");
        return false;
    }
    if (!mqtt->Subscribe(PropertyPostReplyTopic())) {
        ESP_LOGW(TAG, "Subscribe OneNET property post reply topic failed");
    }

    {
        std::lock_guard<std::mutex> lock(g_mqtt_mutex);
        g_mqtt = std::move(mqtt);
    }
    g_mqtt_connected.store(true);
    ESP_LOGI(TAG, "OneNET ready, subscribed topic: %s", PropertySetTopic().c_str());
    return true;
}

bool PublishStm32Data(const McuUartStm32Data& data) {
    bool has_valid_coordinate = data.gps_signal
        && IsValidCoordinate(data.latitude, -90.0, 90.0)
        && IsValidCoordinate(data.longitude, -180.0, 180.0);
    if (data.gps_signal && !has_valid_coordinate) {
        ESP_LOGW(TAG, "Skip invalid coordinate: latitude=%.8f longitude=%.8f",
            data.latitude, data.longitude);
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    std::string message_id = NextMessageId();
    cJSON_AddStringToObject(root, "id", message_id.c_str());

    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON* params = cJSON_CreateObject();
    AddDataPointValue(params, kFallAlarmProperty, data.fall_alarm);
    if (has_valid_coordinate) {
        AddDataPointValue(params, kLatitudeProperty, RoundCoordinate(data.latitude));
        AddDataPointValue(params, kLongitudeProperty, RoundCoordinate(data.longitude));
    }
    cJSON_AddItemToObject(root, "params", params);

    std::string payload = PrintJson(root);
    cJSON_Delete(root);
    if (payload.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mqtt_mutex);
    if (!g_mqtt || !g_mqtt_connected.load()) {
        return false;
    }

    bool ok = g_mqtt->Publish(PropertyPostTopic(), payload);
    ESP_LOGI(TAG, "Publish OneNET %s: %s", ok ? "ok" : "failed", payload.c_str());
    return ok;
}

bool PublishPropertyString(const char* property_name, const std::string& value) {
    if (property_name == nullptr || value.empty()) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    std::string message_id = NextMessageId();
    cJSON_AddStringToObject(root, "id", message_id.c_str());
    cJSON_AddStringToObject(root, "version", "1.0");

    cJSON* params = cJSON_CreateObject();
    AddDataPointValue(params, property_name, value);
    cJSON_AddItemToObject(root, "params", params);

    std::string payload = PrintJson(root);
    cJSON_Delete(root);
    if (payload.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mqtt_mutex);
    if (!g_mqtt || !g_mqtt_connected.load()) {
        ESP_LOGW(TAG, "Publish OneNET failed: MQTT not connected, property=%s", property_name);
        return false;
    }

    bool ok = g_mqtt->Publish(PropertyPostTopic(), payload);
    ESP_LOGI(TAG, "Publish OneNET %s: %s", ok ? "ok" : "failed", payload.c_str());
    return ok;
}

bool Stm32DataChanged(const McuUartStm32Data& current, const McuUartStm32Data& last) {
    if (!last.valid) {
        return true;
    }
    return current.fall_alarm != last.fall_alarm
        || current.gps_signal != last.gps_signal
        || current.gps_status != last.gps_status
        || std::fabs(current.latitude - last.latitude) > kCoordinateChangeThreshold
        || std::fabs(current.longitude - last.longitude) > kCoordinateChangeThreshold;
}

void OnenetTask(void* arg) {
    McuUartStm32Data last_published;
    TickType_t last_publish_tick = 0;

    ESP_LOGI(TAG, "OneNET task running");

    while (true) {
        if (!g_mqtt_connected.load()) {
            {
                std::lock_guard<std::mutex> lock(g_mqtt_mutex);
                g_mqtt.reset();
            }
            ConnectOnenet();
            vTaskDelay(pdMS_TO_TICKS(kOnenetReconnectDelayMs));
            continue;
        }

        McuUartStm32Data current;
        if (mcu_uart_get_latest_stm32_data(&current)) {
            TickType_t now = xTaskGetTickCount();
            bool changed = Stm32DataChanged(current, last_published);
            bool periodic = (now - last_publish_tick) >= pdMS_TO_TICKS(kOnenetPeriodicUploadMs);
            if ((changed || periodic) && PublishStm32Data(current)) {
                last_published = current;
                last_publish_tick = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kOnenetLoopDelayMs));
    }
}
} // namespace

void onenet_client_start() {
    bool expected = false;
    if (!g_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    BaseType_t task_created = xTaskCreate(
        OnenetTask,
        "onenet",
        kOnenetTaskStackSize,
        nullptr,
        kOnenetTaskPriority,
        nullptr);
    if (task_created != pdPASS) {
        g_task_started.store(false);
        ESP_LOGE(TAG, "Failed to create OneNET task");
    } else {
        ESP_LOGI(TAG, "OneNET task started");
    }
}

bool onenet_publish_guai_message(const std::string& message) {
    return PublishPropertyString(kGuaiMessageProperty, message);
}
