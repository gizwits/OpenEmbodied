#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "server/giz_mqtt.h"
#include "websocket_protocol.h"
#include "ssid_manager.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "wifi_station.h"
#include "watchdog.h"

#include "settings.h"
#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "sleeping",
    "audio_testing",
    "fatal_error",
    "power_off",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
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

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒


    auto& board = Board::GetInstance();

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));
            
            display->EnterOTAMode();

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
                display->SetOTAProgress(progress);

#if CONFIG_USE_GIZWITS_MQTT
                auto& mqtt_client = MqttClient::getInstance();
                if (progress == 100) {
                    mqtt_client.sendOtaProgressReport(100, "done");
                    mqtt_client.sendTraceLog("info", "固件升级完成");

                } else {
                    mqtt_client.sendOtaProgressReport(progress, "downloading");
                }
#endif
                
            });

            if (!upgrade_success) {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start(); // Restart audio service
                // 暂时禁用省电模式，避免影响网络连接
        // board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::P3_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            } else {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    Board::GetInstance().WakeUpPowerSaveTimer();

    if (player_.IsDownloading()) {
        CancelPlayMusic();
        return;
    }

    ESP_LOGI(TAG, "ToggleChatState, device_state_:[%d][%s]", device_state_, STATE_STRINGS[device_state_]);
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        // audio_service_.EnableAudioTesting(true);
        // SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        // audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateSleeping) {
        Schedule([this]() {
            auto& board = Board::GetInstance();
            // 还原屏幕亮度
            auto backlight = board.GetBacklight();
            if (backlight) {
                backlight->RestoreBrightness();
            }
            if (!protocol_->IsAudioChannelOpened()) {
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
                SetDeviceState(kDeviceStateConnecting);
            }

            SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
        }, "ToggleChatState_StartListening");
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            ESP_LOGI(TAG, "ToggleChatState(kDeviceStateSpeaking)");
            SetDeviceState(kDeviceStateListening);
        }, "ToggleChatState_AbortSpeaking");
    } else if (device_state_ == kDeviceStateListening) {
        // Schedule([this]() {
        //     protocol_->CloseAudioChannel();
        // });
    }
}

void Application::StartListening() {
    Board::GetInstance().WakeUpPowerSaveTimer();
    CancelPlayMusic();
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        }, "StartListening_OpenChannel");
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);

        }, "StartListening_AbortSpeaking");
    }
}

void Application::StopListening() {
    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }


    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            SetDeviceState(kDeviceStateIdle);
            // 清空上传队列
            audio_service_.ResetSendQueue();
            vTaskDelay(pdMS_TO_TICKS(100));
            protocol_->SendStopListening();
            ESP_LOGI(TAG, "StopListening(kDeviceStateListening)");
        }
    }, "StopListening_SendStop");
}

void Application::Start() {
    auto reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "esp_reset_reason: %d", reset_reason);
    
    // 判断重启类型：ESP_RST_POWERON(1)、ESP_RST_EXT(2)、ESP_RST_SW(3) 为正常重启
    is_normal_reset_ = (reset_reason == ESP_RST_POWERON || 
                        reset_reason == ESP_RST_EXT || 
                        reset_reason == ESP_RST_SW ||
                        reset_reason == ESP_RST_USB ||
                        reset_reason == ESP_RST_JTAG ||
                        reset_reason == ESP_RST_DEEPSLEEP
                    );
    
    if (is_normal_reset_) {
        ESP_LOGI(TAG, "Normal reset detected");
    } else {
        ESP_LOGW(TAG, "Abnormal reset detected - reason: %d", reset_reason);
    }
    
    Settings settings("wifi", true);

#ifdef CONFIG_DEFAULT_CHAT_MODE
    int default_chat_mode = std::stoi(CONFIG_DEFAULT_CHAT_MODE);
    chat_mode_ = settings.GetInt("chat_mode", default_chat_mode); // 0=按键说话, 1=唤醒词, 2=自然对话
#else
    chat_mode_ = settings.GetInt("chat_mode", 1); // 0=按键说话, 1=唤醒词, 2=自然对话
#endif


    auto& board = Board::GetInstance();

    ESP_LOGI(TAG, "chat_mode_: %d", chat_mode_);
    Auth::getInstance().init();
    
    SetDeviceState(kDeviceStateStarting);
    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio service */
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

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);
    tmp_ft_mode_ = settings.GetInt("tmp_ft_mode", 0);

    if (is_normal_reset_ && tmp_ft_mode_ == 0) {
        // 播放上电提示音
        audio_service_.PlaySound(Lang::Sounds::P3_SUCCESS);
    }
    bool can_next = ProductTestCheck();
    if (!can_next) {
        return;
    }

    /* Wait for the network to be ready */
    board.StartNetwork();

    // auto json = board.GetJson();
    // ESP_LOGI(TAG, "json: %s", json.c_str());

    bool battery_ok = CheckBatteryLevel();
    if (!battery_ok) {
        // 播放提示
        vTaskDelay(pdMS_TO_TICKS(3000));
        // Board::GetInstance().PowerOff();
        // return;
    }

    audio_service_.ResetDecoder();
    if (is_normal_reset_) {
        audio_service_.PlaySound(Lang::Sounds::P3_CONNECT_SUCCESS);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // Initialize NTP client
    // auto& ntp_client = NtpClient::GetInstance();
    // esp_err_t ntp_ret = ntp_client.Init();
    // if (ntp_ret == ESP_OK) {
    //     ntp_client.StartSync();
    //     ESP_LOGI(TAG, "NTP client initialized and started");
    // } else {
    //     ESP_LOGE(TAG, "Failed to initialize NTP client: %s", esp_err_to_name(ntp_ret));
    // }
    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
    
    // 先创建protocol_，确保MQTT回调中能安全访问

    protocol_ = std::make_unique<WebsocketProtocol>();

    initGizwitsServer();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // protocol_已经在上面创建过了，不需要重复创建

    protocol_->OnNetworkError([this](const std::string& message) {
        ESP_LOGE(TAG, "OnNetworkError: %s", message.c_str());
        std::string messageData = "socket 通道错误: " + message;
        MqttClient::getInstance().sendTraceLog("info", messageData.c_str());
        last_error_message_ = message;
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            auto packet_ptr = std::make_unique<AudioStreamPacket>(std::move(packet));
            audio_service_.PushPacketToDecodeQueue(std::move(packet_ptr));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        MqttClient::getInstance().sendTraceLog("info", "socket 通道打开");

    });
    protocol_->OnAudioChannelClosed([this, &board](bool is_clean) {
        ESP_LOGW("OnAudioChannelClosed", "is_clean: %d", is_clean);
        if (!is_clean) {
            ESP_LOGW(TAG, "Audio channel closed unexpectedly");
            HandleNetError();
        }
        // 暂时禁用省电模式，避免影响网络连接
        // board.SetPowerSaveMode(true);
        
        if (device_state_ != kDeviceStateSleeping) {
            SetDeviceState(kDeviceStateIdle);
        }

        const char* msg = is_clean ? "socket 通道正常关闭" : "socket 通道异常断开";
        MqttClient::getInstance().sendTraceLog("info", msg);
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        // 有交互
        Board::GetInstance().ResetPowerSaveTimer();
        auto type = cJSON_GetObjectItem(root, "type");
        if (!type || !cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Invalid JSON: missing or invalid 'type' field");
            ESP_LOGW(TAG, "JSON: %s", cJSON_Print(root));
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                if (!has_emotion_) {
                    Schedule([this]() {
                        auto display = Board::GetInstance().GetDisplay();
                        // 没有情绪，则设置为开心
                        display->SetStatus(Lang::Strings::SPEAKING);
                        display->SetEmotion("happy");
                    }, "OnIncomingJson_TTS_Start");
                }
            } else if (strcmp(state->valuestring, "pre_start") == 0) {
                aborted_ = false;
                if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                    SetDeviceState(kDeviceStateSpeaking);
                }
                Schedule([this]() {
                    auto& board = Board::GetInstance();
                    if (board.NeedPlayProcessVoice() && chat_mode_ != 2) {
                        // 自然对话不要 biu
                        ResetDecoder();
                        PlaySound(Lang::Sounds::P3_BO);
                    } else if (board.NeedPlayProcessVoiceWithLife()) {
                        // 不满足上面的条件，但是又开启了自然对话提示音
                        ResetDecoder();
                        PlaySound(Lang::Sounds::P3_BO);
                    }
                    auto display = board.GetDisplay();
                    display->SetEmotion("thinking");
                }, "OnIncomingJson_TTS_PreStart");
                has_emotion_ = false;
                
            } else if (strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI(TAG, "tts stop");
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                }, "OnIncomingJson_TTS_Stop");
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (!text || !cJSON_IsString(text)) {
                    ESP_LOGW(TAG, "Invalid JSON: missing or invalid 'text' field in tts sentence_start");
                    return;
                }
                if (cJSON_IsString(text)) {
                    // ESP_LOGI(TAG, "<< %s", text->valuestring);
                    // Schedule([this, display, message = std::string(text->valuestring)]() {
                    //     display->SetChatMessage("assistant", message.c_str());
                    // }, "OnIncomingJson_TTS_SentenceStart");
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                // ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                }, "OnIncomingJson_STT_SentenceStart");
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                    has_emotion_ = true;
                }, "OnIncomingJson_LLM_Emotion");
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    }, "OnIncomingJson_System_Reboot");
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
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
    bool protocol_started = protocol_->Start();

    SetDeviceState(kDeviceStateIdle);

    if (protocol_started) {
        display->SetEmotion("sleepy");
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();

    StartReportTimer();
}

bool Application::ProductTestCheck() {

    auto& board = Board::GetInstance();
    bool wifi_config_mode_ = board.IsWifiConfigMode();
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();

    /**
    * 生产厂产测模式
      临时产测就不需要进工厂产测了
     */
    if (tmp_ft_mode_ == 0) {
        factory_test_init();
        ESP_LOGI(TAG, "Factory test is enabled: %d", wifi_config_mode_);
        if (wifi_config_mode_ || ssid_list.empty() || factory_test_is_enabled()) {
            ESP_LOGI(TAG, "Factory test start");
            factory_test_start();
            ESP_LOGI(TAG, "Factory test is enabled");
        
            if (factory_test_is_enabled()) {
                ESP_LOGW(TAG, "Factory test is enabled");
                PlaySound(Lang::Sounds::P3_TEST_MODE);
                udp_broadcaster_.async_start();
                return false;
            }
        }
    }
    
    /**
    * 生产厂产测模式
    */

#ifdef CONFIG_TMP_PRODUCT_TEST_WIFI
    /**
    * 简化的整机厂产测模式
    */
    if (tmp_ft_mode_ == 1) {
        // 消费掉flag
        Settings settings("wifi", true);
        settings.SetInt("tmp_ft_mode", 0);
        // PlaySound(Lang::Sounds::P3_TEST_MODE);
        audio_service_.EnableWakeWordDetection(true);
        audio_service_.EnableVoiceProcessing(false);    
        // 临时连接WiFi
        auto& wifi_station = WifiStation::GetInstance();
        // 启动WiFi模块
        wifi_station.Start();
        // 临时连接到产测WiFi（不保存凭据）
        if (wifi_station.ConnectToWifiAndWait(CONFIG_TMP_PRODUCT_TEST_WIFI, CONFIG_TMP_PRODUCT_TEST_WIFI_PASSWORD, 15000)) {
          
            audio_service_.PlaySound(Lang::Sounds::P3_CONNECT_SUCCESS);
        } else {
            ESP_LOGE(TAG, "Failed to connect to test WiFi: %s", CONFIG_TMP_PRODUCT_TEST_WIFI);
        }

        return false;
    }
    /**
    * 简化的整机厂产测模式
    */
#endif

    return true;
}
void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

            // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback, const std::string& task_name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::make_pair(task_name, std::move(callback)));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    auto& watchdog = Watchdog::GetInstance();
    const TickType_t timeout = pdMS_TO_TICKS(300);  // 减少到100ms，提高响应性
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    static int loop_counter = 0;
    
    while (true) {
        auto loop_start = esp_timer_get_time();
        watchdog.Reset();
        loop_counter++;

        // Process NTP sync - 每10次循环执行一次
        if (loop_counter % 10 == 0) {
            auto& ntp_client = NtpClient::GetInstance();
            ntp_client.ProcessSync();
        }

        
#if CONFIG_IDF_TARGET_ESP32C2
// 处理 MQTT 消息队列（替代独立任务）
// 这样做的好处：
// 1. 减少内存占用 - 不需要为每个任务分配独立的栈空间
// 2. 简化任务管理 - 减少任务切换的开销
// 3. 更好的控制 - 在主循环中可以更好地控制执行频率
auto& mqtt_client = MqttClient::getInstance();
if (mqtt_client.isInitialized()) {
    mqtt_client.processMessageQueue();  // 处理接收到的消息
    mqtt_client.processSendQueue();     // 处理待发送的消息
}
#endif
        
        // 每30秒检查一次电量
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_battery_check_time_).count();
        if (duration >= 30) {
            CheckBatteryLevel();
            last_battery_check_time_ = now;
        }

        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, timeout);
        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            auto send_start = esp_timer_get_time();
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                // 尝试发送音频包
                bool sent = protocol_->SendAudio(*packet);
                if (!sent) {
                    // 如果发送失败（比如被忽略），主动清理 packet 内存
                    if (!packet->payload.empty()) {
                        ESP_LOGD(TAG, "Clearing unsent packet payload: %u bytes", (unsigned int)packet->payload.size());
                        packet->payload.clear();
                        packet->payload.shrink_to_fit();
                    }
                }
            }
            auto send_end = esp_timer_get_time();
            ESP_LOGD(TAG, "SendAudio took %lld us", send_end - send_start);
        }

#ifndef CONFIG_USE_EYE_STYLE_VB6824
        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            auto wake_start = esp_timer_get_time();
            OnWakeWordDetected();
            auto wake_end = esp_timer_get_time();
            ESP_LOGD(TAG, "WakeWordDetected took %lld us", wake_end - wake_start);
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }
#endif

        if (bits & MAIN_EVENT_SCHEDULE) {
            auto schedule_start = esp_timer_get_time();
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                watchdog.Reset();
                const auto& name = task.first;
                if (name != "unknown") {
                    ESP_LOGI(TAG, "Executing scheduled task: %s", name.c_str());
                }
                auto task_start = esp_timer_get_time();
                task.second();
                auto task_end = esp_timer_get_time();
                if (name != "unknown") {
                    ESP_LOGI(TAG, "Scheduled task completed: %s, took %lld us", name.c_str(), task_end - task_start);
                }
            }
        }
        
        // 打印整个循环的执行时间
        auto loop_end = esp_timer_get_time();
        auto loop_duration = loop_end - loop_start;
        
        // 只在循环时间超过10ms时打印，避免日志过多
        if (loop_duration > 10000) {  // 10ms
            ESP_LOGD(TAG, "Main loop took %lld us (%.2f ms)", loop_duration, loop_duration / 1000.0);
        }
    }
}

void Application::OnWakeWordDetected() {
    if (chat_mode_ == 0) {
        ESP_LOGI(TAG, "OnWakeWordDetected: chat_mode_ == 0");
        return;
    }
    Board::GetInstance().WakeUpPowerSaveTimer();
    ESP_LOGI(TAG, "OnWakeWordDetected");
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        // while (auto packet = audio_service_.PopWakeWordPacket()) {
        //     protocol_->SendAudio(*packet);
        // }
        // // Set the chat state to wake word detected
        // protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
#else
        SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
        // Play the pop up sound to indicate the wake word is detected
        // audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
        SetDeviceState(kDeviceStateListening);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if(protocol_){
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    ESP_LOGI(TAG, "SetDeviceState: %s -> %s", STATE_STRINGS[device_state_], STATE_STRINGS[state]);
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    // DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();


    if (state != kDeviceStateIdle) {
        // 防止黑屏讲话
        if (board.GetBacklight()) {
            board.GetBacklight()->RestoreBrightness();
        }
    }

    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("sleepy");
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
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                ESP_LOGI(TAG, "SetDeviceState_Listening_SendStartListening");
                // audio_service_.EnableVoiceProcessingWithRetry(true, false, 5000);
                if (chat_mode_ == 0) {
                    // 立即进入聆听模式
                    audio_service_.EnableVoiceProcessing(true, true);
                } else {
                    audio_service_.EnableVoiceProcessing(true, false);
                }

                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateSleeping: {
            // 休眠模式：关闭屏幕、socket、wifi
            ESP_LOGI(TAG, "Entering sleep mode");
            
            
            break;
        }
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateUpgrading:
        case kDeviceStateActivating:
        case kDeviceStateFatalError:
            // 这些状态不需要特殊处理
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
#if CONFIG_USE_GIZWITS_MQTT
    auto& mqtt_client = MqttClient::getInstance();
    mqtt_client.sendTraceLog("info", "唤醒词触发");
#endif
    ESP_LOGI(TAG, "Wake word invoke: %s device_state_: %s chat_mode_: %d", wake_word.c_str(), STATE_STRINGS[device_state_], chat_mode_);
    
    if (IsTmpFactoryTestMode()) {
        // 临时测试模式，播放提示音
        PlaySound(Lang::Sounds::P3_SUCCESS);
        return;
    }
    
    // 内部会判断
    CancelPlayMusic();
     // 按钮模式
     if (chat_mode_ == 0) {
        return;
    }

    if (chat_mode_ == 2 && device_state_ != kDeviceStateIdle) {
        // 自然对话模式，聊天中的时候唤醒词不需要工作
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this, wake_word]() {
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::P3_IM_IN);

            ToggleChatState();
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
            auto& board = Board::GetInstance();
            auto backlight = board.GetBacklight();
            if (backlight) {
                backlight->RestoreBrightness();
            }
        }, "WakeWordInvoke_StartListening");
    } else if (device_state_ == kDeviceStateSpeaking) {
        
        ESP_LOGI(TAG, "WakeWordInvoke(kDeviceStateListening)");
        SetDeviceState(kDeviceStateListening);
        Schedule([this]() {
            // 打断AI
            ESP_LOGI(TAG, "WakeWordInvoke(kDeviceStateSpeaking)");
            protocol_->SendAbortSpeaking(kAbortReasonNone);
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::P3_IM_IN);
            
        }, "WakeWordInvoke_AbortSpeaking");
    } else if (device_state_ == kDeviceStateListening) { 
        ESP_LOGI(TAG, "WakeWordInvoke(kDeviceStateListening): PreAbortSpeaking");
        protocol_->PreAbortSpeaking();
        Schedule([this]() {
            ResetDecoder();
            PlaySound(Lang::Sounds::P3_IM_IN);
            SetDeviceState(kDeviceStateListening);
        });
    } else if (device_state_ == kDeviceStateSleeping) {
        Schedule([this]() {
            ExitSleepMode();
        }, "WakeWordInvoke_ExitSleepMode");
    }
}

bool Application::CanEnterSleepMode() {
    // if (device_state_ != kDeviceStateIdle) {
    //     return false;
    // }
    if (factory_test_is_enabled()) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioCanEnterSleepMode()) {
        return false;
    }
    if (player_.IsDownloading()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Schedule([this, payload]() {
    //     if (protocol_) {
    //         protocol_->SendMcpMessage(payload);
    //     }
    // });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::initGizwitsServer() {
    Settings settings("wifi", true);
#if CONFIG_USE_GIZWITS_MQTT
    auto& mqtt_client = MqttClient::getInstance();
    mqtt_client.OnRoomParamsUpdated([this](const RoomParams& params, bool is_mutual) {
        // 判断 protocol_ 是否启动
        // 如果启动了，就断开重新连接
        if (protocol_->IsAudioChannelOpened()) {
            // 先停止所有正在进行的操作
            Schedule([this, is_mutual]() {
                QuitTalking();
                if (!is_mutual) {
                    ResetDecoder();
                    PlaySound(Lang::Sounds::P3_CONFIG_SUCCESS);
                }
            }, "initGizwitsServer_QuitTalking");
        } else {
            if (!protocol_->GetRoomParams().access_token.empty() && device_state_ != kDeviceStateSleeping) {
                if (!is_mutual) {
                    ResetDecoder();
                    PlaySound(Lang::Sounds::P3_CONFIG_SUCCESS);
                }
            }
        }
        MqttClient::getInstance().sendTraceLog("info", "获取配置智能体成功");
        
        protocol_->UpdateRoomParams(params);
        if((device_state_ == kDeviceStateSleeping || !is_normal_reset_) && chat_mode_ != 0) {
            Schedule([this]() {
                // 直接连接
                SetDeviceState(kDeviceStateConnecting);
                vTaskDelay(pdMS_TO_TICKS(500));
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
                ESP_LOGI(TAG, "initGizwitsServer_OpenAudioChannel_EnableVoiceProcessing");
                ResetDecoder();
                SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
            }, "initGizwitsServer_OpenAudioChannel");
        }
    });

    if (!mqtt_client.initialize()) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        Alert(Lang::Strings::ERROR, Lang::Strings::ERROR, "sad", Lang::Sounds::P3_EXCLAMATION);
        return;
    }
    mqtt_client.connect();
#else


    bool need_activation = settings.GetInt("need_activation");
    bool has_authkey = !Auth::getInstance().getAuthKey().empty();

    if(need_activation == 1) {
        if (!has_authkey) {
            GServer::activationLimitDevice([this, &settings](mqtt_config_t* config) {
                ESP_LOGI(TAG, "Device ID: %s", config->device_id);
                settings.SetString("did", config->device_id);
                settings.SetInt("need_activation", 0);
                GServer::getWebsocketConfig([this](RoomParams* config) {
                    if (config) {
                        protocol_->UpdateRoomParams(*config);
                    }
                });
            });
        } else {
            GServer::activationDevice([this, &settings](mqtt_config_t* config) {
                settings.SetInt("need_activation", 0);
                GServer::getWebsocketConfig([this](RoomParams* config) {
                    if (config) {
                        protocol_->UpdateRoomParams(*config);
                    }
                });
            });
        }
    } else {
        GServer::getWebsocketConfig([this](RoomParams* config) {
            if (config) {
                protocol_->UpdateRoomParams(*config);
            }
        });
    }

    if (!has_authkey) {
        if (need_activation == 1) {
            GServer::activationLimitDevice([this, &settings](mqtt_config_t* config) {
                ESP_LOGI(TAG, "Device ID: %s", config->device_id);
                settings.SetString("did", config->device_id);
                settings.SetInt("need_activation", 0);
                GServer::getWebsocketConfig([this](RoomParams* config) {
                    if (config) {
                        protocol_->UpdateRoomParams(*config);
                    }
                });
            });
            
        } else {
            
        }
        
    } else {
        if (need_activation == 1) {
            ESP_LOGI(TAG, "need_activation is true");
            // 调用注册
            GServer::activationDevice([this, &settings](mqtt_config_t* config) {
                settings.SetInt("need_activation", 0);
                GServer::getWebsocketConfig([this](RoomParams* config) {
                    if (config) {
                        protocol_->UpdateRoomParams(*config);
                    }
                });
            });
        }
    }
#endif

}

void Application::StartReportTimer() {
    if (report_timer_handle_ != nullptr) {
        return;
    }
#if CONFIG_USE_GIZWITS_MQTT
    // 先上报一次
    auto& mqtt_client = MqttClient::getInstance();
    mqtt_client.ReportTimer();
    esp_timer_create_args_t report_timer_args = {
        .callback = [](void* arg) {
            MqttClient::getInstance().ReportTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "report_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&report_timer_args, &report_timer_handle_);
    esp_timer_start_periodic(report_timer_handle_, 2000000);
#endif
}

void Application::SetChatMode(int mode) {
    chat_mode_ = mode;
    Settings settings("wifi", true);
    settings.SetInt("chat_mode", mode);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::CheckBatteryLevel() {
    // 检查电量
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        // ESP_LOGI(TAG, "current Battery level: %d, charging: %d, discharging: %d", level, charging, discharging);
        if (level <= 15 && discharging) {
            // 电量
            PlaySound(Lang::Sounds::P3_BATTLE_LOW);
            return false;
        }
    }
    return true; // 电池电量正常
}


void Application::EnterSleepMode() {
    ESP_LOGI(TAG, "Entering sleep mode");
   

    Schedule([this]() {
        // 关闭 mqtt
         // 关闭屏幕背光
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        auto backlight = board.GetBacklight();

        auto codec = Board::GetInstance().GetAudioCodec();
        codec->EnableOutput(true);
        PlaySound(Lang::Sounds::P3_SLEEP);
        auto& mqtt_client = MqttClient::getInstance();
        mqtt_client.disconnect();
        vTaskDelay(pdMS_TO_TICKS(1500));
        // 关闭 socket 连接
        QuitTalking();
        vTaskDelay(pdMS_TO_TICKS(1000));

        // 关闭 wifi
        auto& wifi_station = WifiStation::GetInstance();
        wifi_station.Stop();
        SetDeviceState(kDeviceStateSleeping);

    
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("sleepy");
        if (backlight) {
            backlight->SetBrightness(0);
        }

        // 启动唤醒词
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        
    }, "EnterSleepMode_SetStatus");
    
}

void Application::ExitSleepMode() {
    ESP_LOGI(TAG, "Exiting sleep mode");
    // 恢复屏幕背光
   
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto backlight = board.GetBacklight();
        if (backlight) {
            backlight->RestoreBrightness();
        }
        // 提示音
        auto codec = Board::GetInstance().GetAudioCodec();
        codec->EnableOutput(true);
        PlaySound(Lang::Sounds::P3_WAKE_UP);
        
        // 重新连接 wifi
        auto& wifi_station = WifiStation::GetInstance();
        if (!wifi_station.IsConnected()) {
            ESP_LOGI(TAG, "Reconnecting to WiFi...");
            wifi_station.Start();
            
            // 等待 wifi 连接
            if (!wifi_station.WaitForConnected(30 * 1000)) { // 30秒超时
                ESP_LOGE(TAG, "Failed to reconnect to WiFi");
                return;
            }
        }

        // 重新连接mqtt
        auto& mqtt_client = MqttClient::getInstance();
        mqtt_client.connect();
        Board::GetInstance().WakeUpPowerSaveTimer();
    }, "ExitSleepMode_RestoreBrightness");
    
}

void Application::HandleNetError() {
    ESP_LOGE(TAG, "HandleNetError");
    QuitTalking();
    ESP_LOGE(TAG, "HandleNetError2");
    ResetDecoder();
    PlaySound(Lang::Sounds::P3_NET_ERR);
}
void Application::SendTextToAI(const std::string& text) {
    if (protocol_) {
        protocol_->SendTextToAI(text);
    }
}

void Application::ResetDecoder() {
    audio_service_.ResetDecoder();
}

void Application::QuitTalking() {
    // 不要使用这个判断protocol_->IsAudioChannelOpened 
    // 因为长时间没有使用 socket处于超时状态
    if (protocol_ != nullptr) {
        ESP_LOGI(TAG, "run close audio channel");
        // 先发送中止消息
        protocol_->SendAbortSpeaking(kAbortReasonNone);
        
        // 关闭音频通道（可能阻塞，但这是必要的清理操作）
        protocol_->CloseAudioChannel();
    }

    ESP_LOGI(TAG, "QuitTalking SetDeviceState(kDeviceStateIdle)");
    SetDeviceState(kDeviceStateIdle);
    
    // 启用唤醒词检测
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "QuitTalking EnableWakeWordDetection");
}


void Application::PlayMusic(const char* url) {
    std::string url_str(url);
    if (url_str.substr(0, 6) == "https:") {
        url_str = "http:" + url_str.substr(6);
    }
    // 新增：如果以 .mp3 结尾，替换为 .p3
    if (url_str.size() >= 4 && url_str.substr(url_str.size() - 4) == ".mp3") {
        url_str.replace(url_str.size() - 4, 4, ".p3");
    }
    QuitTalking();

    player_.setPacketCallback([this](const std::vector<uint8_t>& data) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->payload = data;
        packet->sample_rate = 16000;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    });
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::SPEAKING);
    display->SetEmotion("happy");
    // 单独起一个 task
    struct PlayMusicTaskArgs {
        Application* app;
        std::string url;
    };
    auto* args = new PlayMusicTaskArgs{this, url_str};
    xTaskCreate([](void* arg) {
        auto* args = static_cast<PlayMusicTaskArgs*>(arg);
        args->app->player_.processMP3Stream(args->url.c_str());
        delete args;
        vTaskDelete(NULL);
    }, "process_mp3_stream", 4096, args, 4, nullptr);

}

void Application::CancelPlayMusic() {
    ESP_LOGI(TAG, "CancelPlayMusic, device_state_: %d", player_.IsDownloading());
    if (player_.IsDownloading()) {
        ESP_LOGI(TAG, "Stopping player");
        player_.stop();
    }
}


void Application::GenerateTraceId() {
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    for (int i = 0; i < 16; i++) {
        sprintf(trace_id_ + i * 2, "%02x", random_bytes[i]);
    }
    trace_id_[32] = '\0';
    ESP_LOGI(TAG, "Generated trace ID: %s", trace_id_);
}


// 工厂测试录制相关方法实现
int Application::StartRecordTest(int duration_seconds) {
    ESP_LOGI(TAG, "StartRecordTest: duration=%d seconds", duration_seconds);
    
    std::lock_guard<std::mutex> lock(record_test_mutex_);
    
    if (record_test_active_) {
        ESP_LOGW(TAG, "Record test already active");
        return -1;
    }
    
    // 初始化录制参数
    record_test_active_ = true;
    record_test_duration_seconds_ = duration_seconds;
    record_test_start_time_ = std::chrono::steady_clock::now();
    recorded_audio_data_.clear();
    // 开始录制：打开音频测试分支，使 AudioInputTask 采集并追加数据
    audio_service_.EnableAudioTesting(true);
    // 启动超时计时器，到时自动停止录制
    if (record_timer_handle_) {
        esp_timer_stop(record_timer_handle_);
        esp_timer_delete(record_timer_handle_);
        record_timer_handle_ = nullptr;
    }
    esp_timer_create_args_t args = {
        .callback = [](void* arg){
            Application::GetInstance().StopRecordTest();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "record_timeout",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &record_timer_handle_);
    esp_timer_start_once(record_timer_handle_, (uint64_t)duration_seconds * 1000000ULL);
    
    ESP_LOGI(TAG, "Record test started, will record for %d seconds", duration_seconds);
    return 0;
}

int Application::StartPlayTest(int duration_seconds) {
    ESP_LOGI(TAG, "StartPlayTest: duration=%d seconds", duration_seconds);

    
    if (play_test_active_) {
        ESP_LOGW(TAG, "Play test already active");
        return -1;
    }
    
    if (recorded_audio_data_.empty()) {
        ESP_LOGW(TAG, "No recorded audio data to play");
        return -1;
    }
    
    // 初始化播放参数
    play_test_active_ = true;
    play_test_duration_seconds_ = duration_seconds;
    play_test_start_time_ = std::chrono::steady_clock::now();
    play_test_data_index_ = 0;
    
    // 使用 AudioService 的解码队列播放，避免本地再次创建解码器
    const int frame_duration_ms = 20;   // VB6824 录制固定 20ms 一包
    const int sample_rate_hz = 16000;
    ESP_LOGI(TAG, "Total recorded data size: %zu bytes", recorded_audio_data_.size());

    size_t opus_packet_size = 40; // 每包40字节
    size_t total_packets = recorded_audio_data_.size() / opus_packet_size;
    ESP_LOGI(TAG, "Using opus packet size: %u bytes, total packets: %u", (unsigned)opus_packet_size, (unsigned)total_packets);

    auto& audio_service = GetAudioService();
    size_t enqueued = 0;
    for (size_t i = 0; i + opus_packet_size <= recorded_audio_data_.size(); i += opus_packet_size) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate_hz;
        packet->frame_duration = frame_duration_ms;
        packet->timestamp = 0;
        packet->payload.resize(opus_packet_size);
        memcpy(packet->payload.data(), &recorded_audio_data_[i], opus_packet_size);
        if (audio_service.PushPacketToDecodeQueue(std::move(packet), true)) {
            ++enqueued;
            vTaskDelay(pdMS_TO_TICKS(15));
        } else {
            ESP_LOGW(TAG, "Decode queue full, packet dropped at index %u", (unsigned)i);
        }
    }
    ESP_LOGI(TAG, "Enqueued %u/%u packets to decode queue", (unsigned)enqueued, (unsigned)total_packets);
    
    play_test_active_ = false;
    return 0;
}

void Application::StopRecordTest() {
    ESP_LOGI(TAG, "StopRecordTest");
    
    std::lock_guard<std::mutex> lock(record_test_mutex_);
    
    if (!record_test_active_) {
        ESP_LOGW(TAG, "Record test not active");
        return;
    }
    
    record_test_active_ = false;
    // 结束录制：关闭音频测试分支，停止采集
    audio_service_.EnableAudioTesting(false);
    if (record_timer_handle_) {
        esp_timer_stop(record_timer_handle_);
        esp_timer_delete(record_timer_handle_);
        record_timer_handle_ = nullptr;
    }
    
    // 输出录制结果
    
    // 计算录制的实际时长
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - record_test_start_time_).count();
    ESP_LOGI(TAG, "Actual recording duration");
    
    // 保留录制数据，便于后续播放测试
}

void Application::AppendRecordedAudioData(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(record_test_mutex_);
    if (!record_test_active_) {
        return;
    }
    recorded_audio_data_.insert(recorded_audio_data_.end(), data, data + size);
}