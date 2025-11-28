#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"
#include <queue>

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    // Initialize caching variables - using shared variables from header
    cached_packet_count_ = 0;
    // is_first_packet_, is_start_progress_, is_detect_emotion_ are now member variables from header
    packet_cache_.reserve(MAX_CACHED_PACKETS);
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(const AudioStreamPacket& packet) {
    // if (need_abort_speaking_) {
    //     ESP_LOGI(TAG, "SendAudio: ignore audio");
    //     return false;
    // }
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet.payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet.timestamp);
        bp2->payload_size = htonl(packet.payload.size());
        memcpy(bp2->payload, packet.payload.data(), packet.payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet.payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet.payload.size());
        memcpy(bp3->payload, packet.payload.data(), packet.payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet.payload.data(), packet.payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    // Clear packet cache when closing audio channel (inlined logic)
    busy_sending_audio_ = true;
    packet_cache_.clear();
    cached_packet_count_ = 0;
    is_first_packet_ = false;
    is_start_progress_ = false;
    tts_start_received_ = false;  // 重置 TTS start 状态
    ESP_LOGD(TAG, "Packet cache cleared");
    
    websocket_.reset();
}


bool WebsocketProtocol::HasErrorOccurred() const {
    return error_occurred_;
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    
    // Initialize caching variables for new connection (inlined logic)
    packet_cache_.clear();
    cached_packet_count_ = 0;
    is_first_packet_ = true;
    is_start_progress_ = false;
    abort_speaking_recorded_ = false;  // 重置打断记录状态

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    websocket_->SetHeader("Tenant-Id", CONFIG_CUSTOM_TENANT_ID);

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // 检查是否在打断AI说话后的1秒内，如果是则忽略音频
            if (abort_speaking_recorded_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - abort_speaking_timestamp_).count();
                
                if (elapsed < 1500) {
                    ESP_LOGD(TAG, "Ignoring server audio, elapsed: %lld ms since abort speaking", elapsed);
                    return;
                } else {
                    // 超过1秒后，清除记录
                    abort_speaking_recorded_ = false;
                    ESP_LOGD(TAG, "Audio ignore period ended, elapsed: %lld ms", elapsed);
                }
            }
            
            if (on_incoming_audio_ != nullptr) {
                AudioStreamPacket packet;
                
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    packet = AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    };
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    packet = AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    };
                } else {
                    packet = AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    };
                }
                
                // Implement packet caching logic
                if (is_first_packet_) {
                    is_first_packet_ = false;
                    // Start caching mode, cache MAX_CACHED_PACKETS packets first
                    cached_packet_count_ = 0;
                    packet_cache_.clear();
                }
                
#ifdef CONFIG_BOARD_TYPE_ESP32S3_XUNGUAN_AMOLED_1_28
                // 测试：丢弃前4个音频包
                // 可能百度有坑，前三个包会带上之前说的话，所以需要丢弃
                if (cached_packet_count_ < IGNORE_FIRST_PACKETS) {
                    cached_packet_count_++;
                    ESP_LOGI(TAG, "[TEST] Discarding packet %d/5", cached_packet_count_);
                    return;  // 直接返回，不处理这个包
                }
#endif

                
                if (cached_packet_count_ < MAX_CACHED_PACKETS) {
                    // Still in caching phase, add to cache (从第6个包开始缓存)
                    packet_cache_.push_back(packet);
                    cached_packet_count_++;
                    ESP_LOGD(TAG, "Caching packet %d/%d", cached_packet_count_, MAX_CACHED_PACKETS);
                } else {
                    // Cache is full, start pushing
                    if (!packet_cache_.empty()) {
                        // First push all cached packets
                        for (auto& cached_packet : packet_cache_) {
                            on_incoming_audio_(std::move(cached_packet));
                        }
                        packet_cache_.clear();
                        ESP_LOGI(TAG, "Pushed %d cached packets", cached_packet_count_);
                    }
                    // Push current packet
                    on_incoming_audio_(std::move(packet));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            if (!root) {
                ESP_LOGE(TAG, "JSON parse failed: %.*s", (int)len, data);
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            {
                char* compact = cJSON_PrintUnformatted(root);
                if (compact) {
                    ESP_LOGI(TAG, "Received message type: %s", compact);
                    cJSON_free(compact);
                }
            }
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "stt") == 0) {
                    char message_buffer[256];
                    snprintf(message_buffer, sizeof(message_buffer), 
                        "{\"type\":\"tts\",\"state\":\"pre_start\"}");
                    auto message_json = cJSON_Parse(message_buffer);
                    if (message_json) {
                        on_incoming_json_(message_json);
                        cJSON_Delete(message_json);
                    }
                } else if (strcmp(type->valuestring, "tts") == 0) {
                    // Handle TTS events
                    auto state = cJSON_GetObjectItem(root, "state");
                    if (cJSON_IsString(state)) {
                        if (strcmp(state->valuestring, "start") == 0) {
                            // 过滤重复的 start 事件：如果已经收到 start，必须等到 stop 后才能处理下一次 start
                            if (tts_start_received_) {
                                ESP_LOGW(TAG, "TTS start event ignored: already in start state, waiting for stop");
                                cJSON_Delete(root);
                                return;
                            }
                            
                            ESP_LOGI(TAG, "TTS start event detected");
                            tts_start_received_ = true;  // 标记已收到 start
                            
                            // Reset caching state (inlined logic)
                            is_first_packet_ = true;
                            is_start_progress_ = false;
                            cached_packet_count_ = 0;
                            packet_cache_.clear();
                            ESP_LOGD(TAG, "Caching state reset");

                        } else if (strcmp(state->valuestring, "stop") == 0) {
                            ESP_LOGI(TAG, "TTS stop event detected");
                            tts_start_received_ = false;  // 重置状态，允许下一次 start
                            // 重置打断记录状态，因为对话已结束
                            abort_speaking_recorded_ = false;
                        }
                    }
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %.*s", (int)len, data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this](bool is_clean) {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_(is_clean);
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    cJSON* input_params = cJSON_CreateObject();
    cJSON_AddStringToObject(input_params, "format", "opus");
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    cJSON_AddNumberToObject(input_params, "frame_duration", 20);
#else
    cJSON_AddNumberToObject(input_params, "frame_duration", 60);
#endif
    cJSON_AddItemToObject(root, "input_params", input_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

bool WebsocketProtocol::IsAudioCanEnterSleepMode() const {
    // 休眠才需要判断 timeout
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::SendTextToAI(const std::string& message) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
    "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + message + "\"}";
    SendText(json);
}

void WebsocketProtocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void WebsocketProtocol::HandleReconnect() {
    // Xiaozhi version doesn't need reconnect logic for now
    ESP_LOGI(TAG, "HandleReconnect called - not implemented in xiaozhi version");
}


