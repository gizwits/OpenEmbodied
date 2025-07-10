#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include <esp_wifi.h>
#include "watchdog.h"
#include "assets/lang_config.h"
#include "server/giz_mqtt.h"
#include "auth.h"
#include "mcp_server.h"
#include "settings.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#endif

#include <cstring>

#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <utility>

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
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();
    Settings settings("wifi", false);
    chat_mode_ = settings.GetInt("chat_mode", 1); // 0=按键说话, 1=唤醒词, 2=自然对话

    // 初始化看门狗
    // auto& watchdog = Watchdog::GetInstance();
    // watchdog.Initialize(20, true);  // 10秒超时，超时后触发系统复位

#if (defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32C3))
#if (defined(CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS) && defined(CONFIG_USE_AUDIO_CODEC_DECODE_OPUS))
    background_task_ = new BackgroundTask(2048);
#elif (defined(CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS))
    background_task_ = new BackgroundTask(4096 * 2 + 768);
#else
    background_task_ = new BackgroundTask(4096 * 6 + 2048);
#endif
#else
    background_task_ = new BackgroundTask(4096 * 8);
#endif

    GenerateTraceId();

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#endif

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
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota_.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota_.GetCheckVersionUrl().c_str());
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

        if (ota_.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota_.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StopDetection();
#endif
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec();
            codec->EnableInput(false);
            codec->EnableOutput(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_decode_queue_.clear();
            }
            background_task_->WaitForCompletion();
            delete background_task_;
            background_task_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));

            ota_.StartUpgrade([this,display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);

                if (mqtt_client_) {
                    if (progress == 100) {
                        mqtt_client_->sendOtaProgressReport(100, "done");
                        mqtt_client_->sendTraceLog("info", "固件升级完成");
    
                    } else {
                        mqtt_client_->sendOtaProgressReport(progress, "downloading");
                    }
                }
                
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            Reboot();
            return;
        }

        // No new version, mark the current version as valid
        ota_.MarkCurrentVersionValid();
        if (!ota_.HasActivationCode() && !ota_.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_.HasActivationCode()) {
            ShowActivationCode();
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
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

void Application::ShowActivationCode() {
    auto& message = ota_.GetActivationMessage();
    auto& code = ota_.GetActivationCode();

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
            PlaySound(it->sound);
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
        ResetDecoder();
        PlaySound(sound);
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

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    // The assets are encoded at 16000Hz, 60ms frame duration
    SetDecodeSampleRate(16000, 60);
    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}

void Application::ToggleChatState() {

    if (player_.IsDownloading()) {
        CancelPlayMusic();
        return;
    }

    ESP_LOGI(TAG, "ToggleChatState, device_state_: %d", device_state_);
    if (device_state_ == kDeviceStateActivating) {
        ESP_LOGI(TAG, "ToggleChatState(kDeviceStateActivating)");
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            ESP_LOGI(TAG, "ToggleChatState(kDeviceStateIdle)");
            auto& board = Board::GetInstance();
            // 还原屏幕亮度
            auto backlight = board.GetBacklight();
            if (backlight) {
                backlight->RestoreBrightness();
            }
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                return;
            }

            SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            ESP_LOGI(TAG, "ToggleChatState(kDeviceStateSpeaking)");
            SetDeviceState(kDeviceStateListening);
        });
    } else if (device_state_ == kDeviceStateListening) {
        // Schedule([this]() {
        //     protocol_->CloseAudioChannel();
        // });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        ESP_LOGI(TAG, "StartListening(kDeviceStateActivating)");
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
                ESP_LOGI(TAG, "StartListening(kDeviceStateIdle)");
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "StartListening(kDeviceStateSpeaking)");
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::SendMessage(const std::string& message) {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->SendMessage(message);
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
            protocol_->SendStopListening();
            ESP_LOGI(TAG, "StopListening(kDeviceStateListening)");
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    // auto& watchdog = Watchdog::GetInstance();

    auto& board = Board::GetInstance();
    Auth::getInstance().init();
    
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
#else
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
#else
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    if (chat_mode_ == 2) {
        ESP_LOGI(TAG, "Realtime chat enabled, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
    } else if (board.GetBoardType() == "ml307") {
        ESP_LOGI(TAG, "ML307 board detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
    } else {
        ESP_LOGI(TAG, "WiFi board detected, setting opus encoder complexity to 3");
        opus_encoder_->SetComplexity(3);
    }
#endif

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();


#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        // auto& watchdog = Watchdog::GetInstance();
        // watchdog.SubscribeTask(xTaskGetCurrentTaskHandle());
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);
#else
    // 非 AUDIO_PROCESSOR 就是 6824
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096, this, 8, &audio_loop_task_handle_);
#endif

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();


    PlaySound(Lang::Sounds::P3_CONNECT_SUCCESS);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Initialize MQTT client
    protocol_ = std::make_unique<WebsocketProtocol>();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
#if CONFIG_USE_GIZWITS_MQTT
    mqtt_client_ = std::make_unique<MqttClient>();
    mqtt_client_->OnRoomParamsUpdated([this](const RoomParams& params) {
        // 判断 protocol_ 是否启动
        // 如果启动了，就断开重新连接

        if (protocol_->IsAudioChannelOpened()) {
            // 先停止所有正在进行的操作
            Schedule([this]() {
                QuitTalking();
                PlaySound(Lang::Sounds::P3_CONFIG_SUCCESS);
            });
        } else {
            if (!protocol_->GetRoomParams().access_token.empty()) {
                PlaySound(Lang::Sounds::P3_CONFIG_SUCCESS);
            }
        }
        protocol_->UpdateRoomParams(params);
    });

    if (!mqtt_client_->initialize()) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        Alert(Lang::Strings::ERROR, Lang::Strings::ERROR, "sad", Lang::Sounds::P3_EXCLAMATION);
        return;
    }
#else

    Settings settings("wifi", true);
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

    CheckNewVersion();
    
    // Initialize the protocol
    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
#if CONFIG_IDF_TARGET_ESP32C2
        const int max_packets_in_queue = 3000 / OPUS_FRAME_DURATION_MS;
#else
        const int max_packets_in_queue = 10000 / OPUS_FRAME_DURATION_MS;
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_decode_queue_.size() < max_packets_in_queue) {
            audio_decode_queue_.emplace_back(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate(), protocol_->server_frame_duration());

// #if CONFIG_IOT_PROTOCOL_XIAOZHI
//         auto& thing_manager = iot::ThingManager::GetInstance();
//         protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
//         std::string states;
//         if (thing_manager.GetStatesJson(states, false)) {
//             protocol_->SendIotStates(states);
//         }
// #endif
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
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
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {

                    if (Board::GetInstance().GetServo()) {
                        Board::GetInstance().GetServo()->move(0, 180, 500, 10000000);
                    }

                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                        PlaySound(Lang::Sounds::P3_BO);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    if (Board::GetInstance().GetServo()) {
                        Board::GetInstance().GetServo()->stop();
                    }
                    if (device_state_ == kDeviceStateSpeaking) {
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
                    // ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (cJSON_IsArray(commands)) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
#endif
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
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();


#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_->Initialize(codec, chat_mode_ == 2);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            if (protocol_->IsAudioChannelBusy()) {
                return;
            }
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
                packet.timestamp = last_output_timestamp_;
                last_output_timestamp_ = 0;
                Schedule([this, packet = std::move(packet)]() {
                    protocol_->SendAudio(packet);
                });
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

#endif
    
#if CONFIG_USE_WAKE_WORD_DETECT
    wake_word_detect_.Initialize(codec);
    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {

        Schedule([this, &wake_word]() {
            CancelPlayMusic();
            ESP_LOGI(TAG, "Wake word detected: %s, device state: %s, %d", wake_word.c_str(), STATE_STRINGS[device_state_], device_state_);
            if (device_state_ == kDeviceStateIdle) {
                auto& board = Board::GetInstance();
                // 还原屏幕亮度
                auto backlight = board.GetBacklight();
                if (backlight) {
                    backlight->RestoreBrightness();
                }
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_SUCCESS);

                SetDeviceState(kDeviceStateConnecting);
                wake_word_detect_.EncodeWakeWordData();

                if (!protocol_ || !protocol_->OpenAudioChannel()) {
                    wake_word_detect_.StartDetection();
                    return;
                }

                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
            } else if (device_state_ == kDeviceStateSpeaking) {
                // 关键词打断，继续监听
                protocol_->SendAbortSpeaking(kAbortReasonNone);
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_SUCCESS);
                vTaskDelay(pdMS_TO_TICKS(300));
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                SetListeningMode(chat_mode_ == 2  ? kListeningModeRealtime : kListeningModeAutoStop);
                auto display = Board::GetInstance().GetDisplay();
                display->SetChatMessage("assistant", "");
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }
        });
    });
    wake_word_detect_.StartDetection();
#endif

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
    }
    
    // Enter the main event loop
    // watchdog.SubscribeTask(xTaskGetCurrentTaskHandle());

    StartReportTimer();
    MainEventLoop();
}

void Application::QuitTalking() {
    ESP_LOGI(TAG, "QuitTalking");
    protocol_->SendAbortSpeaking(kAbortReasonNone);
    SetDeviceState(kDeviceStateIdle);
    protocol_->CloseAudioChannel();
    ResetDecoder();
#if CONFIG_USE_WAKE_WORD_DETECT
    wake_word_detect_.StartDetection();
#endif

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
    
    Schedule([this, url_str]() {
        player_.setPacketCallback([this](const std::vector<uint8_t>& data) {
            #if CONFIG_IDF_TARGET_ESP32C2
                const int max_packets_in_queue = 3000 / OPUS_FRAME_DURATION_MS;
            #else
                const int max_packets_in_queue = 10000 / OPUS_FRAME_DURATION_MS;
            #endif
            std::lock_guard<std::mutex> lock(mutex_);
            if (audio_decode_queue_.size() < max_packets_in_queue) {
                AudioStreamPacket packet;
                packet.payload = data;
                audio_decode_queue_.emplace_back(std::move(packet));
            } else {
                ESP_LOGW("AUDIO", "Audio decode queue is full! Current size: %d, Max size: %d", 
                        audio_decode_queue_.size(), max_packets_in_queue);
            }
        });
        player_.processMP3Stream(url_str.c_str());
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::SPEAKING);
        display->SetEmotion("happy");

    });
}

void Application::CancelPlayMusic() {
    ESP_LOGI(TAG, "CancelPlayMusic, device_state_: %d", player_.IsDownloading());
    if (player_.IsDownloading()) {
        ESP_LOGI(TAG, "Stopping player");
        player_.stop();
    }
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // char buffer[500];
        // vTaskList(buffer);
        // ESP_LOGI(TAG, "Task list: \n%s", buffer);
        // SystemInfo::PrintRealTimeStats(pdMS_TO_TICKS(1000));

        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (ota_.HasServerTime()) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    //  auto& watchdog = Watchdog::GetInstance();
    const TickType_t timeout = pdMS_TO_TICKS(3000);
    while (true) {
        // watchdog.Reset();

        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT, pdTRUE, pdFALSE, timeout);

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                // watchdog.Reset();
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    // auto& watchdog = Watchdog::GetInstance();
    while (true) {
        // watchdog.Reset();
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
#if CONFIG_FREERTOS_HZ == 1000
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
    }
}


void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    // if (device_state_ == kDeviceStateListening) {
    //     audio_decode_queue_.clear();
    //     audio_decode_cv_.notify_all();
    //     return;
    // }

    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if(free_sram < 10000){
        return;
    }

    // 打印管道还剩余多少数据
    // ESP_LOGI(TAG, "Audio decode queue size: %d", audio_decode_queue_.size());


    busy_decoding_audio_ = true;
    background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
        WriteAudio(packet.payload);
#else
        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        WriteAudio(pcm, opus_decoder_->sample_rate());
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    });
}

void Application::OnAudioInput() {

    // ESP_LOGI(TAG, "OnAudioInput %d", wake_word_detect_.IsDetectionRunning());
   
#if CONFIG_USE_WAKE_WORD_DETECT
    if (wake_word_detect_.IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_detect_.GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            wake_word_detect_.Feed(data);
            return;
        }
    }
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
    // if (audio_processor_) {
    //     ESP_LOGI(TAG, "Audio processor is running 2, %d", audio_processor_->IsRunning());
    // }
    if (audio_processor_ && audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        // ESP_LOGI(TAG, "Audio processor is running 1");
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            ReadAudio(data, 16000, samples);
            audio_processor_->Feed(data);
            return;
        }
    }
#else

    bool can_read_audio = false;
    if (chat_mode_ == 2) {
        can_read_audio = device_state_ == kDeviceStateListening || realtime_chat_is_start_;
    } else {
        can_read_audio = device_state_ == kDeviceStateListening;
    }
    if (can_read_audio) {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if(free_sram < 10000){
            ESP_LOGE(TAG, "内存不足");
            return;
        }
        
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
        std::vector<uint8_t> opus;
        if (!protocol_->IsAudioChannelBusy()) {
            ReadAudio(opus, 16000, 30 * 16000 / 1000);
            AudioStreamPacket packet;
            packet.payload = std::move(opus);
            packet.timestamp = last_output_timestamp_;
            last_output_timestamp_ = 0;
            protocol_->SendAudio(packet);
        }
#else
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                audio_processor_->Feed(data);
                return;
            }
        }
#endif
        return;
    }
#endif
#if CONFIG_FREERTOS_HZ != 1000
    vTaskDelay(pdMS_TO_TICKS(30));
#endif
}

void Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return;
        }
    }
}


#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
void Application::ReadAudio(std::vector<uint8_t>& opus, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    opus.resize(samples);
    if (!codec->InputData(opus)) {
        return;
    }
}
#endif

void Application::WriteAudio(std::vector<int16_t>& data, int sample_rate) {
    auto codec = Board::GetInstance().GetAudioCodec();
    // Resample if the sample rate is different
    if (sample_rate != codec->output_sample_rate()) {
        int target_size = output_resampler_.GetOutputSamples(data.size());
        std::vector<int16_t> resampled(target_size);
        output_resampler_.Process(data.data(), data.size(), resampled.data());
        data = std::move(resampled);
    }
    codec->OutputData(data);
}

#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
void Application::WriteAudio(std::vector<uint8_t>& opus) {
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->OutputData(opus);
}
#endif

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("sleepy");
#if CONFIG_USE_AUDIO_PROCESSOR
            ESP_LOGI(TAG, "Audio processor audio_processor_->Stop SetDeviceState");
            audio_processor_->Stop();
#endif
            
#if CONFIG_USE_WAKE_WORD_DETECT
            wake_word_detect_.StartDetection();
#endif
            if (chat_mode_ == 2) {
                realtime_chat_is_start_ = false;
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            timestamp_queue_.clear();
            last_output_timestamp_ = 0;
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("relaxed");
            // Update the IoT states before sending the start listening command
#if CONFIG_IOT_PROTOCOL_XIAOZHI
            UpdateIotStates();
#endif

#ifdef CONFIG_USE_AUDIO_PROCESSOR
            // Make sure the audio processor is running
            if (audio_processor_) {
#else
                if(chat_mode_ == 2 && realtime_chat_is_start_){
                    break;
                }
#endif
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                if (listening_mode_ == kListeningModeAutoStop && previous_state == kDeviceStateSpeaking) {
                    // FIXME: Wait for the speaker to empty the buffer
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
#else
                opus_encoder_->ResetState();
#endif

#ifdef CONFIG_USE_WAKE_WORD_DETECT
                ESP_LOGI(TAG, "Stop wake word detection");
                wake_word_detect_.StopDetection();
#endif

#ifdef CONFIG_USE_AUDIO_PROCESSOR
                ESP_LOGI(TAG, "Audio processor audio_processor_->Start SetDeviceState");
                audio_processor_->Start();
            }
#endif
            if (chat_mode_ == 2) {
                realtime_chat_is_start_ = true;
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetEmotion("happy");


            if (listening_mode_ != kListeningModeRealtime) {
#if CONFIG_USE_AUDIO_PROCESSOR
                ESP_LOGI(TAG, "Audio processor audio_processor_->Stop SetDeviceState");
                if (audio_processor_) audio_processor_->Stop();
#endif
#if CONFIG_USE_WAKE_WORD_DETECT
                ESP_LOGI(TAG, "Start wake word detection");
                wake_word_detect_.StartDetection();
#endif
                if (chat_mode_ == 2) {
                    realtime_chat_is_start_ = false;
                }
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::SendTextToAI(const std::string& text) {
    if (protocol_) {
        protocol_->SendTextToAI(text);
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (mqtt_client_) {
        mqtt_client_->sendTraceLog("info", "唤醒词触发");
    }
    ESP_LOGI(TAG, "Wake word invoke: %s", wake_word.c_str());

    // 按钮模式
    if (chat_mode_ == 0) {
        return;
    }
    if (device_state_ == kDeviceStateIdle) {
        
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
            auto& board = Board::GetInstance();
            auto backlight = board.GetBacklight();
            if (backlight) {
                backlight->RestoreBrightness();
            }
            PlaySound(Lang::Sounds::P3_SUCCESS);
            // CancelPlayMusic();
            vTaskDelay(pdMS_TO_TICKS(300));
            ToggleChatState();
            
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            // 打断AI
            ESP_LOGI(TAG, "WakeWordInvoke(kDeviceStateSpeaking)");
            protocol_->SendAbortSpeaking(kAbortReasonNone);
            ResetDecoder();
            PlaySound(Lang::Sounds::P3_SUCCESS);
            ESP_LOGI(TAG, "WakeWordInvoke(kDeviceStateListening)");
            SetDeviceState(kDeviceStateListening);
        });
    } else if (device_state_ == kDeviceStateListening) { 
        // Schedule([this]() {
        //     ResetDecoder();
        //     PlaySound(Lang::Sounds::P3_SUCCESS);
        // });
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


bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (player_.IsDownloading()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            // protocol_->SendMcpMessage(payload);
        }
    });
}


void Application::StartReportTimer() {
    if (report_timer_handle_ != nullptr) {
        return;
    }
    // 先上报一次
    if (mqtt_client_) {
        mqtt_client_->ReportTimer();
        esp_timer_create_args_t report_timer_args = {
            .callback = [](void* arg) {
                static_cast<Application*>(arg)->mqtt_client_->ReportTimer();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "report_timer",
            .skip_unhandled_events = true
        };
        esp_timer_create(&report_timer_args, &report_timer_handle_);
        esp_timer_start_periodic(report_timer_handle_, 60000000); // 1分钟
    }
    
}

void Application::SetChatMode(int mode) {
    chat_mode_ = mode;
    Settings settings("wifi", true);
    settings.SetInt("chat_mode", mode);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}