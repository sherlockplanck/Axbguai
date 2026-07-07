#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "mcu_uart.h"
#include "onenet_client.h"
#include "assets.h"
#include "settings.h"

#include <cstdint>
#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {
constexpr char kMemoNamespace[] = "voice_memo";
constexpr char kMemoContentKey[] = "content";
constexpr char kFamilyMessageNamespace[] = "family_msg";
constexpr char kFamilyMessageContentKey[] = "content";
constexpr size_t kMaxMemoBytes = 3000;
constexpr size_t kMaxFamilyMessageBytes = 512;
constexpr size_t kMaxGuaiMessageBytes = 128;
constexpr int64_t kLocalCommandTtsSuppressUs = 15 * 1000000LL;
constexpr int kFamilyMessageNoticeSampleRate = 24000;

extern const char family_message_notice_pcm_start[] asm("_binary_family_message_notice_pcm_start");
extern const char family_message_notice_pcm_end[] asm("_binary_family_message_notice_pcm_end");

std::string RemoveCommandPunctuation(std::string text) {
    const char* tokens[] = {
        " ", "\t", "\r", "\n", ",", ".", "!", "?", ";", ":",
        "\"", "'", "~",
        "，", "。", "！", "？", "；", "：", "、",
        "“", "”", "‘", "’", "～", "…"
    };
    for (const auto* token : tokens) {
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.erase(pos, strlen(token));
        }
    }
    return text;
}

bool ContainsText(const std::string& text, const char* keyword) {
    return text.find(keyword) != std::string::npos;
}

bool IsMemoStartCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "备忘录" ||
           normalized == "打开备忘录" ||
           normalized == "开启备忘录" ||
           normalized == "开始备忘录" ||
           normalized == "进入备忘录模式";
}

bool IsMemoEndCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "结束" ||
           normalized == "结束备忘录" ||
           normalized == "退出备忘录" ||
           normalized == "停止备忘录";
}

bool IsMemoViewCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "查看备忘录" ||
           normalized == "播放备忘录" ||
           normalized == "读备忘录" ||
           normalized == "朗读备忘录" ||
           ContainsText(normalized, "查看备忘录") ||
           ContainsText(normalized, "播放备忘录");
}

bool IsMemoClearCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "清空备忘录" ||
           normalized == "删除备忘录" ||
           normalized == "清除备忘录" ||
           ContainsText(normalized, "清空备忘录") ||
           ContainsText(normalized, "删除备忘录") ||
           ContainsText(normalized, "清除备忘录");
}

bool IsGuaiMessageStartCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "发消息" ||
           normalized == "发送消息" ||
           normalized == "我要发消息";
}

bool IsFamilyMessageViewCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "查看留言" ||
           normalized == "播放留言" ||
           normalized == "读留言" ||
           normalized == "朗读留言" ||
           ContainsText(normalized, "查看留言") ||
           ContainsText(normalized, "播放留言");
}

bool IsCurrentLocationCommand(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    return normalized == "我现在在哪里" ||
           normalized == "我在哪" ||
           normalized == "我在哪里" ||
           normalized == "当前位置" ||
           normalized == "我的位置" ||
           normalized == "当前定位" ||
           normalized == "我在什么地方" ||
           ContainsText(normalized, "我现在在哪里") ||
           ContainsText(normalized, "当前位置") ||
           ContainsText(normalized, "我的位置") ||
           ContainsText(normalized, "当前定位");
}

std::string ExtractCallContactName(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    const char* markers[] = {
        "打电话给",
        "拨打电话给"
    };

    for (const auto* marker : markers) {
        size_t pos = normalized.find(marker);
        if (pos != std::string::npos) {
            std::string name = normalized.substr(pos + strlen(marker));
            const char* suffixes[] = { "吧", "吗" };
            for (const auto* suffix : suffixes) {
                size_t suffix_len = strlen(suffix);
                if (name.size() >= suffix_len
                    && name.compare(name.size() - suffix_len, suffix_len, suffix) == 0) {
                    name.erase(name.size() - suffix_len);
                }
            }
            return name;
        }
    }
    return "";
}

bool HasCoordinateText(const std::string& text) {
    return (ContainsText(text, "经度") && ContainsText(text, "纬度")) ||
           ContainsText(text, "经纬度");
}

bool IsUsefulLocationAnswer(const std::string& text) {
    if (HasCoordinateText(text)) {
        return true;
    }

    bool has_poi_detail = ContainsText(text, "「") && ContainsText(text, "」");
    bool looks_like_address = ContainsText(text, "位置") ||
                              ContainsText(text, "你在") ||
                              ContainsText(text, "你现在在") ||
                              ContainsText(text, "你刚刚在") ||
                              ContainsText(text, "你还是在") ||
                              ContainsText(text, "当前位置");
    bool has_address_detail = ContainsText(text, "省") ||
                              ContainsText(text, "市") ||
                              ContainsText(text, "区") ||
                              ContainsText(text, "县") ||
                              ContainsText(text, "街道");
    return looks_like_address && (has_address_detail || has_poi_detail);
}

bool IsUsefulFamilyMessageAnswer(const std::string& text, const std::string& expected_message) {
    auto normalized_text = RemoveCommandPunctuation(text);
    auto normalized_expected = RemoveCommandPunctuation(expected_message);

    if (normalized_expected.empty() || normalized_expected == "当前没有留言") {
        return normalized_text == "当前没有留言" || normalized_text == "没有留言";
    }

    if (normalized_text == normalized_expected) {
        return true;
    }

    const char* wrappers[] = {
        "留言是",
        "留言内容是",
        "内容是"
    };
    for (const auto* wrapper : wrappers) {
        std::string wrapped = std::string(wrapper) + normalized_expected;
        if (normalized_text == wrapped ||
            normalized_text == wrapped + "啦" ||
            normalized_text == wrapped + "哦") {
            return true;
        }
    }

    return false;
}

std::string NormalizeWakeWordText(const std::string& text) {
    auto normalized = RemoveCommandPunctuation(text);
    if (normalized == "hi小智" || normalized == "Hi小智" || normalized == "HI小智") {
        return "你好小智";
    }
    return text;
}

void TrimMemoToLimit(std::string& memo) {
    if (memo.size() <= kMaxMemoBytes) {
        return;
    }

    memo.erase(0, memo.size() - kMaxMemoBytes);
    while (!memo.empty() && (static_cast<unsigned char>(memo.front()) & 0xC0) == 0x80) {
        memo.erase(0, 1);
    }
}

void TrimFamilyMessageToLimit(std::string& message) {
    if (message.size() <= kMaxFamilyMessageBytes) {
        return;
    }

    message.erase(kMaxFamilyMessageBytes);
    while (!message.empty() && (static_cast<unsigned char>(message.back()) & 0xC0) == 0x80) {
        message.pop_back();
    }
}

void TrimGuaiMessageToLimit(std::string& message) {
    if (message.size() <= kMaxGuaiMessageBytes) {
        return;
    }

    message.erase(kMaxGuaiMessageBytes);
    while (!message.empty()) {
        size_t char_start = message.size() - 1;
        while (char_start > 0 && (static_cast<unsigned char>(message[char_start]) & 0xC0) == 0x80) {
            --char_start;
        }

        unsigned char lead = static_cast<unsigned char>(message[char_start]);
        size_t char_len = 1;
        if ((lead & 0x80) == 0x00) {
            char_len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            char_len = 4;
        } else {
            message.erase(char_start);
            continue;
        }

        if (message.size() - char_start >= char_len) {
            break;
        }
        message.erase(char_start);
    }
}
} // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    onenet_client_start();
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (!ShouldSuppressLocalCommandTts() &&
            !suppress_current_location_tts_sentence_ &&
            !suppress_family_message_tts_sentence_ &&
            GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (ShouldSuppressLocalCommandTts()) {
                ESP_LOGI(TAG, "Suppressing local-command TTS state: %s", state->valuestring);
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                suppress_current_location_tts_sentence_ = false;
                suppress_family_message_tts_sentence_ = false;
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                filter_current_location_tts_ = false;
                suppress_current_location_tts_sentence_ = false;
                filter_family_message_tts_ = false;
                suppress_family_message_tts_sentence_ = false;
                family_message_tts_text_.clear();
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::string tts_text(text->valuestring);
                    bool preserve_current_location_display = false;
                    if (filter_current_location_tts_) {
                        bool has_coordinates = HasCoordinateText(tts_text);
                        bool allow_sentence = IsUsefulLocationAnswer(tts_text);
                        suppress_current_location_tts_sentence_ = !allow_sentence;
                        if (!allow_sentence) {
                            ESP_LOGI(TAG, "Suppressing non-location TTS: %s", tts_text.c_str());
                            return;
                        }
                        preserve_current_location_display = !has_coordinates;
                    }
                    if (filter_family_message_tts_) {
                        bool allow_sentence = IsUsefulFamilyMessageAnswer(tts_text, family_message_tts_text_);
                        suppress_family_message_tts_sentence_ = !allow_sentence;
                        if (!allow_sentence) {
                            ESP_LOGI(TAG, "Suppressing stale family-message TTS: %s", tts_text.c_str());
                            return;
                        }
                    }
                    ESP_LOGI(TAG, "<< %s", tts_text.c_str());
                    if (!preserve_current_location_display) {
                        Schedule([display, message = tts_text]() {
                            display->SetChatMessage("assistant", message.c_str());
                        });
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::string recognized_text = NormalizeWakeWordText(text->valuestring);
                ESP_LOGI(TAG, ">> %s", recognized_text.c_str());
                auto final = cJSON_GetObjectItem(root, "is_final");
                bool is_final = !cJSON_IsBool(final) || cJSON_IsTrue(final);

                bool handled_locally = false;
                if (is_final) {
                    if (IsCurrentLocationCommand(recognized_text)) {
                        filter_current_location_tts_ = true;
                        suppress_current_location_tts_sentence_ = false;
                    }

                    handled_locally = HandleCallContactVoiceCommand(recognized_text);

                    if (!handled_locally && recognized_text.find("打电话给紧急联系人") != std::string::npos) {
                        ESP_LOGI(TAG, "检测到自定义指令：打电话给紧急联系人，正在下发串口...");
                        mcu_uart_send_esp32_cmd(true, "emergency_contact");
                        handled_locally = true;
                        SuppressLocalCommandTts();
                        Schedule([]() {
                            auto display = Board::GetInstance().GetDisplay();
                            display->SetChatMessage("system", "已发送紧急联系人呼叫指令");
                            display->ShowNotification("已发送紧急联系人呼叫指令", 3000);
                        });
                    } else if (recognized_text.find("发送信息") != std::string::npos) {
                        ESP_LOGI(TAG, "检测到自定义指令：启动任务，正在下发串口...");
                        mcu_uart_send_esp32_cmd(true, "start");
                    } else if (recognized_text.find("停止任务") != std::string::npos) {
                        ESP_LOGI(TAG, "检测到自定义指令：停止任务，正在下发串口...");
                        mcu_uart_send_esp32_cmd(false, "stop");
                    }

                    if (!handled_locally) {
                        handled_locally = HandleGuaiMessageVoiceCommand(recognized_text);
                    }
                    if (!handled_locally) {
                        handled_locally = HandleFamilyMessageVoiceCommand(recognized_text);
                    }
                    if (!handled_locally) {
                        handled_locally = HandleMemoVoiceCommand(recognized_text);
                    }
                    if (!handled_locally) {
                        suppress_local_tts_until_us_ = 0;
                    }
                }

                if (!handled_locally) {
                    Schedule([display, message = recognized_text]() {
                        display->SetChatMessage("user", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = NormalizeWakeWordText(audio_service_.GetLastWakeWord());
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = NormalizeWakeWordText(audio_service_.GetLastWakeWord());

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

bool Application::HandleMemoVoiceCommand(const std::string& text) {
    if (memo_recording_) {
        if (IsMemoEndCommand(text)) {
            FinishMemoRecording();
        } else {
            AppendMemoText(text);
        }
        return true;
    }

    if (IsMemoViewCommand(text)) {
        PlayStoredMemo();
        return true;
    }

    if (IsMemoClearCommand(text)) {
        ClearStoredMemo();
        return true;
    }

    if (IsMemoStartCommand(text)) {
        StartMemoRecording();
        return true;
    }

    return false;
}

bool Application::HandleGuaiMessageVoiceCommand(const std::string& text) {
    if (guai_message_recording_) {
        if (IsMemoEndCommand(text)) {
            FinishGuaiMessageRecording();
        } else {
            AppendGuaiMessageText(text);
        }
        return true;
    }

    if (IsGuaiMessageStartCommand(text)) {
        StartGuaiMessageRecording();
        return true;
    }

    return false;
}

bool Application::HandleCallContactVoiceCommand(const std::string& text) {
    std::string requested_name = ExtractCallContactName(text);
    if (requested_name.empty()) {
        return false;
    }

    std::string name1;
    std::string name2;
    {
        std::lock_guard<std::mutex> lock(call_contact_mutex_);
        name1 = call_contact_name1_;
        name2 = call_contact_name2_;
    }

    std::string normalized_requested = RemoveCommandPunctuation(requested_name);
    std::string normalized_name1 = RemoveCommandPunctuation(name1);
    std::string normalized_name2 = RemoveCommandPunctuation(name2);
    int call_cmd = 0;
    if (!normalized_name1.empty() && normalized_requested == normalized_name1) {
        call_cmd = 1;
    } else if (!normalized_name2.empty() && normalized_requested == normalized_name2) {
        call_cmd = 2;
    }
    if (call_cmd == 0) {
        return false;
    }

    esp_err_t ret = mcu_uart_send_call_cmd(call_cmd);
    ESP_LOGI(TAG, "Call contact voice command matched: %s call_cmd=%d", requested_name.c_str(), call_cmd);
    SuppressLocalCommandTts();
    Schedule([requested_name, ok = (ret == ESP_OK)]() {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = ok ? "已发送拨号指令：" : "拨号指令发送失败：";
        message += requested_name;
        display->SetChatMessage("system", message.c_str());
        display->ShowNotification(message.c_str(), 3000);
    });
    return true;
}

bool Application::HandleFamilyMessageVoiceCommand(const std::string& text) {
    if (!IsFamilyMessageViewCommand(text)) {
        return false;
    }

    family_message_tts_text_ = LoadFamilyMessage();
    if (family_message_tts_text_.empty()) {
        family_message_tts_text_ = "当前没有留言";
    }
    filter_family_message_tts_ = true;
    suppress_family_message_tts_sentence_ = false;

    PlayStoredFamilyMessage();
    return true;
}

bool Application::ShouldSuppressLocalCommandTts() const {
    return suppress_local_tts_until_us_ > 0 && esp_timer_get_time() < suppress_local_tts_until_us_;
}

void Application::SuppressLocalCommandTts() {
    suppress_local_tts_until_us_ = esp_timer_get_time() + kLocalCommandTtsSuppressUs;
    if (protocol_) {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
    }
    ResumeListeningAfterLocalCommand();
}

void Application::ResumeListeningAfterLocalCommand() {
    Schedule([this]() {
        if (!protocol_) {
            return;
        }

        auto mode = GetDefaultListeningMode();
        listening_mode_ = mode;

        auto state = GetDeviceState();
        if (!protocol_->IsAudioChannelOpened()) {
            if (state != kDeviceStateIdle && state != kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }

        if (state == kDeviceStateSpeaking || state == kDeviceStateIdle) {
            audio_service_.ResetDecoder();
            SetDeviceState(kDeviceStateListening);
        } else if (state == kDeviceStateListening) {
            protocol_->SendStartListening(listening_mode_);
            audio_service_.EnableVoiceProcessing(true);
        }
    });
}

void Application::StartMemoRecording() {
    memo_recording_ = true;
    memo_draft_.clear();
    ESP_LOGI(TAG, "Memo recording started");
    SuppressLocalCommandTts();

    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("备忘录模式已开启，请开始说内容，说“结束”保存", 5000);
        display->SetChatMessage("system", "备忘录模式已开启");
    });
}

void Application::AppendMemoText(const std::string& text) {
    if (text.empty()) {
        return;
    }

    if (!memo_draft_.empty()) {
        memo_draft_ += "\n";
    }
    memo_draft_ += text;
    TrimMemoToLimit(memo_draft_);
    ESP_LOGI(TAG, "Memo appended: %s", text.c_str());
    SuppressLocalCommandTts();

    Schedule([message = std::string("已记录：") + text]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", message.c_str());
    });
}

void Application::FinishMemoRecording() {
    memo_recording_ = false;

    std::string message;
    if (memo_draft_.empty()) {
        message = "备忘录已结束，没有新增内容";
    } else {
        std::string memo = LoadMemo();
        if (!memo.empty()) {
            memo += "\n";
        }
        memo += memo_draft_;
        TrimMemoToLimit(memo);
        SaveMemo(memo);
        memo_draft_.clear();
        message = "备忘录已保存";
    }

    ESP_LOGI(TAG, "%s", message.c_str());
    SuppressLocalCommandTts();

    Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(message.c_str(), 5000);
        display->SetChatMessage("system", message.c_str());
    });
}

void Application::StartGuaiMessageRecording() {
    guai_message_recording_ = true;
    guai_message_draft_.clear();
    ESP_LOGI(TAG, "Guai message recording started");
    SuppressLocalCommandTts();

    Schedule([]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification("发消息模式已开启，说完内容后说“结束”发送", 5000);
        display->SetChatMessage("system", "发消息模式已开启");
    });
}

void Application::AppendGuaiMessageText(const std::string& text) {
    if (text.empty()) {
        return;
    }

    if (!guai_message_draft_.empty()) {
        guai_message_draft_ += "\n";
    }
    guai_message_draft_ += text;
    TrimGuaiMessageToLimit(guai_message_draft_);
    ESP_LOGI(TAG, "Guai message appended: %s", text.c_str());
    SuppressLocalCommandTts();

    Schedule([message = std::string("待发送：") + guai_message_draft_]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", message.c_str());
    });
}

void Application::FinishGuaiMessageRecording() {
    guai_message_recording_ = false;

    std::string message;
    if (guai_message_draft_.empty()) {
        message = "消息为空，未发送";
    } else {
        TrimGuaiMessageToLimit(guai_message_draft_);
        bool ok = onenet_publish_guai_message(guai_message_draft_);
        ESP_LOGI(TAG, "Guai message publish %s: %s", ok ? "ok" : "failed", guai_message_draft_.c_str());
        message = ok ? "消息已发送到云端" : "消息发送失败，请检查 OneNET 连接";
        guai_message_draft_.clear();
    }

    SuppressLocalCommandTts();

    Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(message.c_str(), 5000);
        display->SetChatMessage("system", message.c_str());
    });
}

void Application::PlayStoredMemo() {
    suppress_local_tts_until_us_ = 0;

    std::string memo = LoadMemo();
    if (memo.empty()) {
        memo = "当前没有备忘录";
    }

    ESP_LOGI(TAG, "Memo display and speech requested");

    Schedule([memo]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("assistant", memo.c_str());
        display->ShowNotification("正在播报备忘录", 3000);
    });
}

void Application::ClearStoredMemo() {
    SuppressLocalCommandTts();

    Settings settings(kMemoNamespace, true);
    settings.EraseKey(kMemoContentKey);
    memo_draft_.clear();

    ESP_LOGI(TAG, "Memo cleared");

    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", "备忘录已清空");
        display->ShowNotification("备忘录已清空", 3000);
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

std::string Application::LoadMemo() const {
    Settings settings(kMemoNamespace, false);
    return settings.GetString(kMemoContentKey);
}

void Application::SaveMemo(const std::string& memo) {
    Settings settings(kMemoNamespace, true);
    settings.SetString(kMemoContentKey, memo);
}

void Application::OnFamilyMessageReceived(const std::string& message) {
    if (message.empty()) {
        return;
    }

    std::string saved_message = message;
    TrimFamilyMessageToLimit(saved_message);
    SaveFamilyMessage(saved_message);
    ESP_LOGI(TAG, "Family message updated: %s", saved_message.c_str());

    Schedule([this, saved_message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("assistant", "家人给你发来留言");
        display->ShowNotification("家人给你发来留言", 5000);

        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr && codec->output_sample_rate() == kFamilyMessageNoticeSampleRate) {
            auto pcm = reinterpret_cast<const int16_t*>(family_message_notice_pcm_start);
            size_t samples = (family_message_notice_pcm_end - family_message_notice_pcm_start) / sizeof(int16_t);
            audio_service_.PlayPcm(pcm, samples);
        } else if (codec != nullptr) {
            ESP_LOGW(TAG, "Skip family message notice audio: output sample rate is %d, expected %d",
                codec->output_sample_rate(), kFamilyMessageNoticeSampleRate);
        }
    });
}

void Application::OnCallContactNamesReceived(
    const std::string& name1,
    bool has_name1,
    const std::string& name2,
    bool has_name2) {
    {
        std::lock_guard<std::mutex> lock(call_contact_mutex_);
        if (has_name1) {
            call_contact_name1_ = name1;
        }
        if (has_name2) {
            call_contact_name2_ = name2;
        }
    }

    ESP_LOGI(TAG, "Call contact names updated: name1=%s name2=%s",
        has_name1 ? name1.c_str() : "<unchanged>",
        has_name2 ? name2.c_str() : "<unchanged>");
}

void Application::OnMcuAckReceived(const std::string& ack_cmd) {
    std::string message;
    if (ack_cmd == "contact_update") {
        message = "更新成功";
    } else if (ack_cmd == "call") {
        message = "发送成功";
    } else {
        return;
    }

    ESP_LOGI(TAG, "MCU ACK display: cmd=%s message=%s", ack_cmd.c_str(), message.c_str());
    Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", message.c_str());
        display->ShowNotification(message.c_str(), 3000);
    });
}

void Application::PlayStoredFamilyMessage() {
    suppress_local_tts_until_us_ = 0;

    std::string message = LoadFamilyMessage();
    if (message.empty()) {
        message = "当前没有留言";
    }

    ESP_LOGI(TAG, "Family message display and speech requested");

    Schedule([message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("assistant", message.c_str());
        display->ShowNotification("正在播报留言", 3000);
    });
}

std::string Application::LoadFamilyMessage() const {
    Settings settings(kFamilyMessageNamespace, false);
    return settings.GetString(kFamilyMessageContentKey);
}

void Application::SaveFamilyMessage(const std::string& message) {
    Settings settings(kFamilyMessageNamespace, true);
    settings.SetString(kFamilyMessageContentKey, message);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

