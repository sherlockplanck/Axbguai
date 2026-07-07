#include "amap_navigation.h"

#include "board.h"
#include "http.h"
#include "mcu_uart.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

#define TAG "AmapNav"

namespace {
// 高德地图 Web 服务配置：
// 1. 到高德开放平台创建“Web服务”类型 Key，并开通地理编码、路径规划服务。
// 2. 把高德 Key 填到 kDefaultAmapKey；百度 AK 不能用于高德。
// 3. ESP32 资源有限，这里使用 HTTP 而不是 HTTPS，减少 TLS 对实时语音的影响。
// 4. 建议配置默认起点和默认城市，这样用户只说“导航到xxx”也能工作。
// 5. 如果能拿到默认起点经纬度，填 kDefaultOriginLng/kDefaultOriginLat，可少一次地理编码请求。
constexpr char kDefaultAmapKey[] = "f51f39e317cd1ef7332d0dd511184532";
constexpr char kDefaultOrigin[] = "";
constexpr char kDefaultCity[] = "";
constexpr double kDefaultOriginLng = 0.0;
constexpr double kDefaultOriginLat = 0.0;

constexpr char kAmapNamespace[] = "amap";
constexpr int kHttpTimeoutMs = 5000;
constexpr int kMaxRouteSteps = 6;
constexpr size_t kHttpReadBufferSize = 1024;
constexpr size_t kMaxHttpResponseBytes = 20 * 1024;
constexpr size_t kMaxAddressBytes = 96;

struct Coordinate {
    double lat = 0;
    double lng = 0;
};

bool ReadHttpBody(Http& http, std::string& response);

std::string LoadMapSetting(const std::string& key, const char* default_value) {
    Settings settings(kAmapNamespace, false);
    auto value = settings.GetString(key);
    if (value.empty()) {
        value = default_value;
    }
    return value;
}

double LoadMapDoubleSetting(const std::string& key, double default_value) {
    auto value = LoadMapSetting(key, "");
    if (value.empty()) {
        return default_value;
    }
    return std::atof(value.c_str());
}

bool HttpGet(const std::string& url, std::string& response) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "HTTP open failed, err=0x%x", http->GetLastError());
        http->Close();
        return false;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP status: %d", status_code);
        http->Close();
        return false;
    }

    if (!ReadHttpBody(*http, response)) {
        http->Close();
        return false;
    }
    ESP_LOGI(TAG, "HTTP response length: %u", static_cast<unsigned>(response.size()));
    http->Close();
    return !response.empty();
}

std::string UrlEncode(const std::string& input) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

std::string TrimString(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }

    return input.substr(start, end - start);
}

bool IsInvalidAddress(const std::string& address) {
    return address.empty() || address.size() > kMaxAddressBytes;
}

bool IsValidCoordinate(const Coordinate& coordinate) {
    return coordinate.lng >= -180 && coordinate.lng <= 180
        && coordinate.lat >= -90 && coordinate.lat <= 90
        && (coordinate.lng != 0 || coordinate.lat != 0);
}

bool GetLatestUartCoordinate(Coordinate& coordinate) {
    McuUartStm32Data data;
    if (!mcu_uart_get_latest_stm32_data(&data)) {
        return false;
    }
    if (!data.gps_signal) {
        return false;
    }

    coordinate.lng = data.longitude;
    coordinate.lat = data.latitude;
    return IsValidCoordinate(coordinate);
}

bool LooksLikeCompleteJson(const std::string& response) {
    auto start = response.find_first_not_of(" \r\n\t");
    auto end = response.find_last_not_of(" \r\n\t");
    return start != std::string::npos && end != std::string::npos
        && response[start] == '{' && response[end] == '}';
}

bool ReadHttpBody(Http& http, std::string& response) {
    response.clear();

    size_t content_length = http.GetBodyLength();
    if (content_length > kMaxHttpResponseBytes) {
        ESP_LOGE(TAG, "HTTP response too large: %u bytes", static_cast<unsigned>(content_length));
        return false;
    }

    char buffer[kHttpReadBufferSize];
    while (true) {
        int ret = http.Read(buffer, sizeof(buffer));
        if (ret < 0) {
            if (LooksLikeCompleteJson(response)) {
                ESP_LOGW(TAG, "HTTP read ended after complete JSON: %u bytes", static_cast<unsigned>(response.size()));
                return true;
            }
            ESP_LOGE(TAG, "HTTP read failed, received=%u", static_cast<unsigned>(response.size()));
            return false;
        }
        if (ret == 0) {
            break;
        }

        if (response.size() + ret > kMaxHttpResponseBytes) {
            ESP_LOGE(TAG, "HTTP response exceeded limit: %u bytes", static_cast<unsigned>(response.size() + ret));
            return false;
        }

        response.append(buffer, ret);
        if (content_length > 0 && response.size() >= content_length) {
            break;
        }
    }

    return !response.empty();
}

std::string FormatCoordinate(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return buffer;
}

std::string FormatCoordinatePair(const Coordinate& coordinate) {
    return FormatCoordinate(coordinate.lng) + "," + FormatCoordinate(coordinate.lat);
}

std::string NormalizeMode(const std::string& mode) {
    if (mode == "walking" || mode == "步行" || mode == "走路") {
        return "walking";
    }
    if (mode == "riding" || mode == "bicycling" || mode == "骑行" || mode == "骑车" || mode == "自行车") {
        return "bicycling";
    }
    return "driving";
}

std::string ModeName(const std::string& mode) {
    if (mode == "walking") {
        return "步行";
    }
    if (mode == "bicycling") {
        return "骑行";
    }
    return "驾车";
}

int JsonToInt(const cJSON* item) {
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item)) {
        return std::atoi(item->valuestring);
    }
    return 0;
}

std::string FormatDistance(int meters) {
    if (meters >= 1000) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.1f公里", meters / 1000.0);
        return buffer;
    }
    return std::to_string(meters) + "米";
}

std::string FormatDuration(int seconds) {
    int minutes = (seconds + 59) / 60;
    if (minutes >= 60) {
        int hours = minutes / 60;
        int remain_minutes = minutes % 60;
        if (remain_minutes == 0) {
            return std::to_string(hours) + "小时";
        }
        return std::to_string(hours) + "小时" + std::to_string(remain_minutes) + "分钟";
    }
    return std::to_string(std::max(minutes, 1)) + "分钟";
}

std::string StripHtmlTags(const std::string& input) {
    std::string output;
    bool in_tag = false;
    for (char c : input) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            output += c;
        }
    }
    return output;
}

std::string GetJsonString(const cJSON* root, const char* key, const std::string& fallback = "") {
    auto value = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(value)) {
        return value->valuestring;
    }
    return fallback;
}

bool IsAmapStatusOk(const cJSON* root) {
    auto status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status)) {
        return std::string(status->valuestring) == "1";
    }
    if (cJSON_IsNumber(status)) {
        return status->valueint == 1;
    }
    return true;
}

std::string GetAmapErrorMessage(const cJSON* root, const std::string& fallback) {
    auto info = GetJsonString(root, "info");
    auto infocode = GetJsonString(root, "infocode");
    if (!info.empty() && !infocode.empty()) {
        return info + "(" + infocode + ")";
    }
    if (!info.empty()) {
        return info;
    }
    return fallback;
}

bool ParseLocation(const std::string& location, Coordinate& coordinate) {
    auto comma = location.find(',');
    if (comma == std::string::npos) {
        return false;
    }

    coordinate.lng = std::atof(location.substr(0, comma).c_str());
    coordinate.lat = std::atof(location.substr(comma + 1).c_str());
    return IsValidCoordinate(coordinate);
}

bool GeocodeAddress(const std::string& address,
                    const std::string& city,
                    const std::string& key,
                    Coordinate& coordinate,
                    std::string& error) {
    std::string url = "http://restapi.amap.com/v3/geocode/geo?output=json&address=" + UrlEncode(address)
        + "&key=" + UrlEncode(key);
    if (!city.empty()) {
        url += "&city=" + UrlEncode(city);
    }

    std::string response;
    if (!HttpGet(url, response)) {
        error = "请求高德地图地理编码失败";
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        error = "高德地图地理编码返回解析失败";
        return false;
    }

    if (!IsAmapStatusOk(root)) {
        error = "地理编码失败：" + GetAmapErrorMessage(root, "请检查地址或 Key");
        cJSON_Delete(root);
        return false;
    }

    auto geocodes = cJSON_GetObjectItem(root, "geocodes");
    auto geocode = cJSON_IsArray(geocodes) ? cJSON_GetArrayItem(geocodes, 0) : nullptr;
    auto location = cJSON_IsObject(geocode) ? cJSON_GetObjectItem(geocode, "location") : nullptr;
    if (!cJSON_IsString(location) || !ParseLocation(location->valuestring, coordinate)) {
        error = "没有找到地址坐标，请重新说完整地点";
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

bool ReverseGeocodeCoordinate(const Coordinate& coordinate,
                              const std::string& key,
                              std::string& address,
                              std::string& error) {
    std::string url = "http://restapi.amap.com/v3/geocode/regeo?output=json&location="
        + UrlEncode(FormatCoordinatePair(coordinate))
        + "&key=" + UrlEncode(key)
        + "&radius=1000&extensions=base";

    std::string response;
    if (!HttpGet(url, response)) {
        error = "请求高德地图逆地理编码失败";
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        error = "高德地图逆地理编码返回解析失败";
        return false;
    }

    if (!IsAmapStatusOk(root)) {
        error = "逆地理编码失败：" + GetAmapErrorMessage(root, "请检查 Key 或服务权限");
        cJSON_Delete(root);
        return false;
    }

    auto regeocode = cJSON_GetObjectItem(root, "regeocode");
    address = cJSON_IsObject(regeocode) ? GetJsonString(regeocode, "formatted_address") : "";
    if (address.empty()) {
        error = "高德地图没有返回当前位置地址";
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

const cJSON* GetRouteHolder(const cJSON* root, const std::string& mode) {
    if (mode == "bicycling") {
        auto data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            return data;
        }
    }
    auto route = cJSON_GetObjectItem(root, "route");
    if (cJSON_IsObject(route)) {
        return route;
    }
    return cJSON_GetObjectItem(root, "data");
}

std::string BuildRouteSummary(const std::string& response,
                              const std::string& origin,
                              const std::string& destination,
                              const std::string& mode) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return "高德地图路线结果解析失败";
    }

    if (!IsAmapStatusOk(root)) {
        auto message = GetAmapErrorMessage(root, "请检查路线参数或 Key 权限");
        cJSON_Delete(root);
        return "路线规划失败：" + message;
    }

    auto holder = GetRouteHolder(root, mode);
    if (cJSON_IsObject(holder)) {
        auto errcode = cJSON_GetObjectItem(holder, "errcode");
        if (cJSON_IsNumber(errcode) && errcode->valueint != 0) {
            auto errmsg = GetJsonString(holder, "errmsg", "请检查路线参数或 Key 权限");
            auto errdetail = GetJsonString(holder, "errdetail");
            cJSON_Delete(root);
            return errdetail.empty() ? "路线规划失败：" + errmsg : "路线规划失败：" + errmsg + "，" + errdetail;
        }
    }

    auto paths = cJSON_IsObject(holder) ? cJSON_GetObjectItem(holder, "paths") : nullptr;
    auto path = cJSON_IsArray(paths) ? cJSON_GetArrayItem(paths, 0) : nullptr;
    if (!cJSON_IsObject(path)) {
        cJSON_Delete(root);
        return "高德地图没有返回可用路线";
    }

    std::string summary = "从" + origin + "到" + destination + "，" + ModeName(mode);
    int distance = JsonToInt(cJSON_GetObjectItem(path, "distance"));
    int duration = JsonToInt(cJSON_GetObjectItem(path, "duration"));
    if (distance > 0) {
        summary += "约" + FormatDistance(distance);
    }
    if (duration > 0) {
        summary += "，预计" + FormatDuration(duration);
    }

    auto steps = cJSON_GetObjectItem(path, "steps");
    if (cJSON_IsArray(steps) && cJSON_GetArraySize(steps) > 0) {
        summary += "。路线：";
        int count = std::min(cJSON_GetArraySize(steps), kMaxRouteSteps);
        for (int i = 0; i < count; ++i) {
            auto step = cJSON_GetArrayItem(steps, i);
            auto instruction = cJSON_IsObject(step) ? cJSON_GetObjectItem(step, "instruction") : nullptr;
            if (!cJSON_IsString(instruction)) {
                continue;
            }
            if (i > 0) {
                summary += "；";
            }
            summary += StripHtmlTags(instruction->valuestring);
        }
    }

    cJSON_Delete(root);
    return summary;
}
} // namespace

namespace AmapNavigation {

std::string PlanRoute(const std::string& origin,
                      const std::string& destination,
                      const std::string& mode,
                      const std::string& city) {
    auto key = LoadMapSetting("key", kDefaultAmapKey);
    if (key.empty()) {
        return "高德地图 Key 未配置。请在 main/amap_navigation.cc 中填写 kDefaultAmapKey。";
    }

    auto requested_origin = TrimString(origin);
    auto default_origin = LoadMapSetting("default_origin", kDefaultOrigin);
    auto route_origin = TrimString(requested_origin.empty() ? default_origin : requested_origin);
    auto route_destination = TrimString(destination);
    auto route_city = TrimString(city.empty() ? LoadMapSetting("default_city", kDefaultCity) : city);
    if (IsInvalidAddress(route_destination)) {
        return "请提供目的地。";
    }

    Coordinate origin_coordinate;
    bool has_origin_coordinate = false;
    if (requested_origin.empty() && GetLatestUartCoordinate(origin_coordinate)) {
        route_origin = "当前位置";
        has_origin_coordinate = true;
    } else {
        origin_coordinate.lat = LoadMapDoubleSetting("default_origin_lat", kDefaultOriginLat);
        origin_coordinate.lng = LoadMapDoubleSetting("default_origin_lng", kDefaultOriginLng);
        has_origin_coordinate = requested_origin.empty() && IsValidCoordinate(origin_coordinate);
        if (has_origin_coordinate && route_origin.empty()) {
            route_origin = "默认起点";
        }
    }
    if (!has_origin_coordinate && IsInvalidAddress(route_origin)) {
        return "请提供起点，或先让 STM32 通过串口发送有效经纬度。";
    }

    Coordinate destination_coordinate;
    std::string error;

    if (!GeocodeAddress(route_destination, route_city, key, destination_coordinate, error)) {
        return "目的地" + route_destination + error;
    }
    if (!has_origin_coordinate) {
        if (!GeocodeAddress(route_origin, route_city, key, origin_coordinate, error)) {
            return "起点" + route_origin + error;
        }
    }

    auto route_mode = NormalizeMode(mode);
    std::string url;
    if (route_mode == "bicycling") {
        url = "http://restapi.amap.com/v4/direction/bicycling";
    } else {
        url = "http://restapi.amap.com/v3/direction/" + route_mode;
    }
    url += "?origin=" + UrlEncode(FormatCoordinatePair(origin_coordinate))
        + "&destination=" + UrlEncode(FormatCoordinatePair(destination_coordinate))
        + "&output=json&key=" + UrlEncode(key);
    if (route_mode == "driving") {
        url += "&extensions=base&strategy=0";
    } else if (route_mode == "walking") {
        url += "&extensions=base";
    }

    std::string response;
    if (!HttpGet(url, response)) {
        return "请求高德地图路线规划失败";
    }

    return BuildRouteSummary(response, route_origin, route_destination, route_mode);
}

std::string GetCurrentLocation() {
    auto key = LoadMapSetting("key", kDefaultAmapKey);
    if (key.empty()) {
        return "高德地图 Key 未配置。请在 main/amap_navigation.cc 中填写 kDefaultAmapKey。";
    }

    Coordinate coordinate;
    if (!GetLatestUartCoordinate(coordinate)) {
        return "还没有收到 STM32 发送的有效 GPS 定位数据。请先通过串口发送 STM32_DATA,gps_signal=1,longitude=经度,latitude=纬度,fall_alarm=0。";
    }

    std::string address;
    std::string error;
    if (!ReverseGeocodeCoordinate(coordinate, key, address, error)) {
        return error + "，坐标：经度" + FormatCoordinate(coordinate.lng)
            + "，纬度" + FormatCoordinate(coordinate.lat);
    }

    return "位置：" + address + "，坐标：经度"
        + FormatCoordinate(coordinate.lng) + "，纬度" + FormatCoordinate(coordinate.lat);
}

} // namespace AmapNavigation
