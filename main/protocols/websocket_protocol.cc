#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include <optional>

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"
#include "protocols/mcp.h"

#define TAG "WS"

#define MAX_AUDIO_PACKET_SIZE 512
#define MAX_CACHED_PACKETS 10

struct Emotion {
    const char* icon;
    const char* text;
};
static const std::vector<Emotion> emotions = {
    {"😶", "neutral"},
    {"🙂", "happy"},
    {"😆", "laughing"},
    {"😂", "funny"},
    {"😔", "sad"},
    {"😠", "angry"},
    {"😭", "crying"},
    {"😍", "loving"},
    {"😳", "embarrassed"},
    {"😯", "surprised"},
    {"😱", "shocked"},
    {"🤔", "thinking"},
    {"😉", "winking"},
    {"😎", "cool"},
    {"😌", "relaxed"},
    {"🤤", "delicious"},
    {"😘", "kissy"},
    {"😏", "confident"},
    {"😴", "sleepy"},
    {"😜", "silly"},
    {"🙄", "confused"},
    {"🤡", "vertigo"}
};


WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    // 如果有关闭任务正在运行，等待它完成
    if (close_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Waiting for close task to complete...");
        // 等待任务完成，最多等待 3 秒
        TickType_t timeout = pdMS_TO_TICKS(3000);
        while (close_task_handle_ != nullptr && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout -= pdMS_TO_TICKS(100);
        }
        
        if (close_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Close task did not complete in time, force cleanup");
            // 强制清理 websocket
            if (websocket_ != nullptr) {
                delete websocket_;
                websocket_ = nullptr;
            }
        }
    } else if (websocket_ != nullptr) {
        // 如果没有关闭任务，直接清理 websocket
        delete websocket_;
    }
    
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    return true;
}

void WebsocketProtocol::SendAudio(const AudioStreamPacket& packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected() || packet.payload.empty() || busy_sending_audio_) {
        return;
    }
    const std::vector<uint8_t>& data = packet.payload;
    // Calculate required base64 buffer size
    size_t out_len = 4 * ((data.size() + 2) / 3);
    
    // Resize base64 buffer if needed
    if (out_len + 1 > base64_buffer_size_) {
        base64_buffer_.reset(new char[out_len + 1]);
        base64_buffer_size_ = out_len + 1;
        if (!base64_buffer_) {
            ESP_LOGE(TAG, "Failed to allocate base64 buffer");
            return;
        }
    }

    size_t encoded_len;
    mbedtls_base64_encode((unsigned char *)base64_buffer_.get(), base64_buffer_size_, &encoded_len,
                         (const unsigned char*)data.data(), data.size());
    base64_buffer_[encoded_len] = '\0';

    // Create event ID
    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);

    // Reuse message buffer
    message_buffer_.clear();
    message_buffer_.reserve(256 + out_len);  // Pre-allocate space
    message_buffer_ = "{";
    message_buffer_ += "\"id\":\"" + std::string(event_id) + "\",";
    message_buffer_ += "\"event_type\":\"input_audio_buffer.append\",";
    message_buffer_ += "\"data\":{";
    message_buffer_ += "\"delta\":\"" + std::string(base64_buffer_.get()) + "\"";
    message_buffer_ += "}";
    message_buffer_ += "}";

    // Send message
    websocket_->Send(message_buffer_);
}


bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr) {
        return false;
    }
    websocket_->Send(text);
    return true;
}

void WebsocketProtocol::SendStopListening() {
    if (websocket_ == nullptr) {
        return;
    }

    // 创建事件 ID (使用随机数，确保为正数)
    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);

    // 构建完整的消息
    char message[256];
    snprintf(message, sizeof(message),
        "{"
            "\"id\":\"%s\","
            "\"event_type\":\"input_audio_buffer.complete\","
            "\"data\":{}"
        "}", event_id);

    // 发送消息
    websocket_->Send(message);
    ESP_LOGI(TAG, "SendStopListening: %s", message);
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    // if (Application::GetInstance().GetChatMode() == 0) {
    //     return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_;
    // }
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

bool WebsocketProtocol::HasErrorOccurred() const {
    return error_occurred_;
}

void WebsocketProtocol::CloseAudioChannel() {
    if (websocket_ == nullptr) {
        return;
    }

    // 如果已经有关闭任务在运行，直接返回
    if (close_task_handle_ != nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Starting audio channel close task...");
    
    // 启动关闭任务
    BaseType_t ret = xTaskCreate(
        CloseAudioChannelTask,
        "ws_close_task",
        4096,
        this,
        10,
        &close_task_handle_
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create close task");
        // 如果创建任务失败，直接执行关闭逻辑
        CloseAudioChannelTask(this);
    }
}

void WebsocketProtocol::CloseAudioChannelTask(void* param) {
    WebsocketProtocol* self = static_cast<WebsocketProtocol*>(param);
    
    ESP_LOGI(TAG, "Closing audio channel...");
    
    // 1. 先停止音频上传 - 设置标志位防止新的音频数据发送
    self->busy_sending_audio_ = true;
    
    // 2. 等待当前正在传输的音频数据完成
    // 给一些时间让正在传输的数据完成
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // 3. 发送关闭帧给服务器
    if (self->websocket_ != nullptr) {
        self->websocket_->Close();
    }
    
    // 5. 清理资源
    if (self->websocket_ != nullptr) {
        delete self->websocket_;
        self->websocket_ = nullptr;
    }
    
    ESP_LOGI(TAG, "Audio channel closed successfully");
    
    // 6. 清理任务句柄
    self->close_task_handle_ = nullptr;
    
    // 7. 删除任务
    vTaskDelete(nullptr);
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
    }
    if (room_params_.bot_id.empty() || room_params_.access_token.empty() || room_params_.voice_id.empty()) {
        ESP_LOGE(TAG, "Bot ID or access token or voice id is empty");
        return false;
    }
    // 用来标记是否触发 progress
    // 如果触发了，收到第一包音频再进入说话模式
    static bool is_first_packet_ = false;
    static bool is_detect_emotion_ = false;
    error_occurred_ = false;
    busy_sending_audio_ = false;  // 重置音频发送标志
    std::string url = std::string("ws://") + room_params_.api_domain + std::string("/v1/chat") + std::string("?bot_id=") + std::string(room_params_.bot_id);
    std::string token = "Bearer " + std::string(room_params_.access_token);

    message_cache_ = "";
    websocket_ = Board::GetInstance().CreateWebSocket();
    websocket_->SetHeader("Authorization", token.c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (!data || len == 0) {
            return;
        }
        std::string_view str_data(data, len);

        constexpr std::string_view key = "\"event_type\":\"";
        size_t event_start = str_data.find(key);
        if (event_start == std::string_view::npos) {
            return;
        }

        event_start += key.length();
        size_t event_end = str_data.find('"', event_start);
        if (event_end == std::string_view::npos) {
            return;
        }

        std::string_view event_type = str_data.substr(event_start, event_end - event_start);
        if (event_type.empty() || event_type.length() >= 64) {
            return;
        }
        // ESP_LOGI(TAG, "event_type: %.*s", (int)event_type.length(), event_type.data());

        // if(event_type != "chat.created") {
        //     return;
        // }
        if(event_type == "conversation.audio.delta") {

            constexpr std::string_view content_key = "\"content\":\"";
            size_t content_start = str_data.find(content_key);
            if (content_start != std::string_view::npos) {
                content_start += content_key.length();
                
                size_t content_end = content_start;
                bool escaped = false;
                
                while (content_end < str_data.length()) {
                    if (str_data[content_end] == '\\') {
                        escaped = !escaped;
                    } else if (str_data[content_end] == '"' && !escaped) {
                        break;
                    } else {
                        escaped = false;
                    }
                    content_end++;
                }
                
                if (content_end < str_data.length()) {
                    std::string_view base64_content = str_data.substr(content_start, content_end - content_start);
                    
                    // Calculate decoded size
                    size_t output_len = 0;
                    mbedtls_base64_decode(nullptr, 0, &output_len, 
                        (const unsigned char*)base64_content.data(), 
                        base64_content.length());

                    // 只在初始化时分配最大 buffer
                    if (audio_data_buffer_.capacity() < MAX_AUDIO_PACKET_SIZE) {
                        audio_data_buffer_.reserve(MAX_AUDIO_PACKET_SIZE);
                    }
                    // 限制最大长度，防止溢出
                    if (output_len > MAX_AUDIO_PACKET_SIZE) {
                        ESP_LOGW(TAG, "Audio packet too large: %u, truncated to %u", (unsigned)output_len, MAX_AUDIO_PACKET_SIZE);
                        output_len = MAX_AUDIO_PACKET_SIZE;
                    }
                    audio_data_buffer_.resize(output_len);

                    size_t actual_len = 0;
                    int ret = mbedtls_base64_decode(
                        audio_data_buffer_.data(), audio_data_buffer_.size(), &actual_len,
                        (const unsigned char*)base64_content.data(), 
                        base64_content.length());

                    if (ret == 0 && actual_len > 0) {
                        if (on_incoming_audio_ != nullptr) {
                            // 队列长度限制逻辑在下游
                            if (is_first_packet_ == true) {
                                message_buffer_.clear();
                                message_buffer_ = "{";
                                message_buffer_ += "\"type\":\"tts\",";
                                message_buffer_ += "\"state\":\"start\"";
                                message_buffer_ += "}";
                                   
                                auto message_json = cJSON_Parse(message_buffer_.c_str());
                                if (message_json) {
                                    on_incoming_json_(message_json);
                                    cJSON_Delete(message_json);
                                }
                                is_first_packet_ = false;
                                // 开始缓存模式，先缓存MAX_CACHED_PACKETS包数据
                                cached_packet_count_ = 0;
                                packet_cache_.clear();
                            }
                            
                            // 创建当前音频包
                            AudioStreamPacket packet;
                            packet.payload.assign(audio_data_buffer_.begin(), audio_data_buffer_.begin() + actual_len);
                            
                            if (cached_packet_count_ < MAX_CACHED_PACKETS) {
                                // 还在缓存阶段，添加到缓存
                                packet_cache_.push_back(std::move(packet));
                                cached_packet_count_++;
                                ESP_LOGI(TAG, "Caching packet %d/%d", cached_packet_count_, MAX_CACHED_PACKETS);
                            } else {
                                // 缓存已满，开始推送
                                if (!packet_cache_.empty()) {
                                    // 先推送所有缓存的包
                                    for (auto& cached_packet : packet_cache_) {
                                        on_incoming_audio_(std::move(cached_packet));
                                    }
                                    packet_cache_.clear();
                                    ESP_LOGI(TAG, "Pushed %d cached packets", cached_packet_count_);
                                }
                                // 推送当前包
                                on_incoming_audio_(std::move(packet));
                            }
                        }
                    }
                }
            }
        } else {
            // Reuse cJSON root
            cJSON* root = cJSON_Parse(data);
            if (!root) {
                return;
            }

            if (event_type == "chat.created") {
                ParseServerHello(root);

            } else if (event_type == "conversation.chat.created") {
                auto id = cJSON_GetObjectItem(root, "id");
                ESP_LOGI(TAG, "conversation.chat.created: %s", id->valuestring);
                std::string message = "conversation.chat.created: " + std::string(id->valuestring);
                MqttClient::getInstance().sendTraceLog("info", message.c_str());
                
            } else if (event_type == "conversation.audio_transcript.update") {
                auto data_json = cJSON_GetObjectItem(root, "data");
                auto content_json = cJSON_GetObjectItem(data_json, "content");
                
                // Reuse message buffer
                message_buffer_.clear();
                message_buffer_ = "{";
                message_buffer_ += "\"type\":\"stt\",";
                message_buffer_ += "\"text\":\"" + std::string(content_json->valuestring) + "\"";
                message_buffer_ += "}";
                
                auto message_json = cJSON_Parse(message_buffer_.c_str());
                if (message_json) {
                    on_incoming_json_(message_json);
                    cJSON_Delete(message_json);
                }
            } else if (event_type == "conversation.chat.in_progress") {
                ESP_LOGI(TAG, "conversation.chat.in_progress");
                MqttClient::getInstance().sendTraceLog("info", "conversation.chat.in_progress");

                is_detect_emotion_ = false;
                is_first_packet_ = true;
                cached_packet_count_ = 0;
                packet_cache_.clear();

                message_cache_.clear();
                message_buffer_.clear();
                message_buffer_ = "{";
                message_buffer_ += "\"type\":\"tts\",";
                message_buffer_ += "\"state\":\"pre_start\"";
                message_buffer_ += "}";
                
                auto message_json = cJSON_Parse(message_buffer_.c_str());
                if (message_json) {
                    on_incoming_json_(message_json);
                    cJSON_Delete(message_json);
                }
                
            } else if (event_type == "conversation.chat.completed" || event_type == "conversation.audio.completed") {
                is_first_packet_ = false;
                cached_packet_count_ = 0;
                packet_cache_.clear();

                std::string messageData = "conversation.chat.completed or conversation.audio.completed";
                MqttClient::getInstance().sendTraceLog("info", messageData.c_str());

                message_buffer_.clear();
                message_buffer_ = "{";
                message_buffer_ += "\"type\":\"tts\",";
                message_buffer_ += "\"state\":\"stop\"";
                message_buffer_ += "}";
                
                auto message_json = cJSON_Parse(message_buffer_.c_str());
                if (message_json) {
                    on_incoming_json_(message_json);
                    cJSON_Delete(message_json);
                }
            } else if (event_type == "input_audio_buffer.speech_started") {

                MqttClient::getInstance().sendTraceLog("info", "input_audio_buffer.speech_started");
                auto& app = Application::GetInstance();

                ESP_LOGI(TAG, "input_audio_buffer.speech_started");
                // 自然对话才要打断
                int chat_mode = app.GetChatMode();
                if (chat_mode == 2) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
            } else if (event_type == "input_audio_buffer.speech_stopped") {
                MqttClient::getInstance().sendTraceLog("info", "input_audio_buffer.speech_stopped");
                ESP_LOGI(TAG, "input_audio_buffer.speech_stopped");
            } else if (event_type == "conversation.message.delta") {
                auto data_json = cJSON_GetObjectItem(root, "data");
                auto content_json = cJSON_GetObjectItem(data_json, "content");

                message_cache_ += std::string(content_json->valuestring);
                message_buffer_.clear();
                message_buffer_ = "{";
                message_buffer_ += "\"type\":\"tts\",";
                message_buffer_ += "\"state\":\"sentence_start\",";
                message_buffer_ += "\"text\":\"" + message_cache_ + "\"";
                message_buffer_ += "}";
                
                auto message_json = cJSON_Parse(message_buffer_.c_str());
                if (message_json) {
                    on_incoming_json_(message_json);
                    cJSON_Delete(message_json);
                }
                if (is_detect_emotion_ == false) {
                    // 查找 message_cache_ 是否包含 emotions
                    for (const auto& emotion : emotions) {
                        if (message_cache_.find(emotion.icon) != std::string::npos) {
                            is_detect_emotion_ = true;
                            message_buffer_.clear();
                            message_buffer_ = "{";
                            message_buffer_ += "\"type\":\"llm\",";
                            message_buffer_ += "\"emotion\":\"" + std::string(emotion.text) + "\"";
                            message_buffer_ += "}";
                            
                            auto message_json = cJSON_Parse(message_buffer_.c_str());
                            if (message_json) {
                                on_incoming_json_(message_json);
                                cJSON_Delete(message_json);
                            }
                            break;
                        }
                    }
                }
            } else if (event_type == "conversation.chat.requires_action") {
                CozeMCPParser::getInstance().handle_mcp(str_data);
            } else if (event_type == "error") {
                ESP_LOGE(TAG, "Error: %s", str_data.data());
                MqttClient::getInstance().sendTraceLog("error", str_data.data());
            }
            
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s", url.c_str());
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
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

    std::string user_language = "common";
    std::string parameters = "";
    std::string custom_variables = "";
    
    // 如果存在 config，解析其中的参数
    if (!room_params_.config.empty()) {
        cJSON* config_json = cJSON_Parse(room_params_.config.c_str());
        if (config_json) {
            // 提取 user_language
            cJSON* asr_config = cJSON_GetObjectItem(config_json, "asr_config");
            if (asr_config && cJSON_IsObject(asr_config)) {
                cJSON* user_lang_item = cJSON_GetObjectItem(asr_config, "user_language");
                if (user_lang_item && cJSON_IsString(user_lang_item)) {
                    user_language = std::string(user_lang_item->valuestring);
                }
            }
            
                            // 提取 parameters
                cJSON* chat_config = cJSON_GetObjectItem(config_json, "chat_config");
                if (chat_config && cJSON_IsObject(chat_config)) {
                    cJSON* parameters_item = cJSON_GetObjectItem(chat_config, "parameters");
                    if (parameters_item && cJSON_IsObject(parameters_item)) {
                        char* parameters_str = cJSON_PrintUnformatted(parameters_item);
                        if (parameters_str) {
                            parameters = std::string(parameters_str);
                            free(parameters_str);
                        }
                    }
                    
                    // 提取 custom_variables
                    cJSON* custom_variables_item = cJSON_GetObjectItem(chat_config, "custom_variables");
                    if (custom_variables_item && cJSON_IsObject(custom_variables_item)) {
                        char* custom_variables_str = cJSON_PrintUnformatted(custom_variables_item);
                        if (custom_variables_str) {
                            custom_variables = std::string(custom_variables_str);
                            free(custom_variables_str);
                        }
                    }
            }
            cJSON_Delete(config_json);
        }
    }

    
    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);
    Settings settings("wifi", true);
    int speed = Board::GetInstance().GetVoiceSpeed();
    int chat_mode = Application::GetInstance().GetChatMode();
    
    std::string codec = "opus";
    std::string message = "{";
    message += "\"id\":\"" + std::string(event_id) + "\",";
    message += "\"event_type\":\"chat.update\",";
    message += "\"data\":{";
    if (room_params_.need_play_prologue) {
        message += "\"need_play_prologue\":true,";
    }
    message += "\"event_subscriptions\": [";
    message += "\"chat.created\",";
    message += "\"chat.updated\",";
    message += "\"conversation.chat.created\",";
    message += "\"conversation.chat.in_progress\",";
    message += "\"conversation.audio.delta\",";
    message += "\"conversation.audio.completed\",";
    message += "\"conversation.chat.completed\",";
    message += "\"conversation.chat.failed\",";
    message += "\"error\",";
    message += "\"input_audio_buffer.completed\",";
    message += "\"input_audio_buffer.cleared\",";
    message += "\"conversation.cleared\",";
    message += "\"conversation.chat.canceled\",";
    message += "\"conversation.audio_transcript.completed\",";
    message += "\"conversation.chat.requires_action\",";
    message += "\"input_audio_buffer.speech_started\",";

#ifndef CONFIG_IDF_TARGET_ESP32C2
    // C2 处理不过来
    message += "\"conversation.message.delta\",";
#endif
    message += "\"input_audio_buffer.speech_stopped\"";
    message += "],";
    if (chat_mode != 0) {
        message += "\"turn_detection\": {";
        message += "\"type\": \"server_vad\",";  // 判停类型，client_vad/server_vad，默认为 client_vad
        message += "\"prefix_padding_ms\": 300,"; // server_vad模式下，VAD 检测到语音之前要包含的音频量，单位为 ms。默认为 600ms
        message += "\"silence_duration_ms\": 800"; // server_vad模式下，检测语音停止的静音持续时间，单位为 ms。默认为 800ms
        message += "},";
    }
    message += "\"chat_config\":{";
    message += "\"auto_save_history\":true,";
    message += "\"conversation_id\":\"" + room_params_.conv_id + "\",";
    message += "\"user_id\":\"" + room_params_.user_id + "\",";
    if (!parameters.empty()) {
        message += "\"parameters\":" + parameters + ",";
    }
    message += "\"meta_data\":{},";
    if (!custom_variables.empty()) {
        message += "\"custom_variables\":" + custom_variables + ",";
    } else {
        message += "\"custom_variables\":{},";
    }
    message += "\"extra_params\":{}";
    message += "},";
    message += "\"input_audio\":{";
    message += "\"format\":\"pcm\",";
    message += "\"codec\":\"opus\",";
    message += "\"sample_rate\":16000,";
    message += "\"channel\":1,";
    message += "\"bit_depth\":16";
    message += "},";
    message += "\"asr_config\":{\"user_language\":\"" + user_language + "\"},";
    message += "\"output_audio\":{";
    message += "\"codec\":\"" + codec + "\",";
    message += "\"opus_config\":{";
    message += "\"sample_rate\":16000,";
    message += "\"use_cbr\":false,";
    message += "\"frame_size_ms\":60,";
    message += "\"limit_config\":{";
    message += "\"period\":1,";
#if CONFIG_IDF_TARGET_ESP32C2
    message += "\"max_frame_num\":17";
#else
    message += "\"max_frame_num\":20";
#endif
    message += "}";
    message += "},";
    message += "\"speech_rate\":" + std::to_string(speed) + ",";
    message += "\"voice_id\":\"" + std::string(room_params_.voice_id) + "\"";
    message += "}";
    message += "}";
    message += "}";
    
    websocket_->Send(message);
    // ESP_LOGI(TAG, "Send message: %s", message.c_str());
    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    return "";
}

void WebsocketProtocol::SendTextToAI(const std::string& text) {
    std::string message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"" + text + "\""
        "}"
    "}";
    // 如果需要 const char* 类型
    const char* message_cstr = message.c_str();
    SendText(message_cstr);
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    // COZE 的音频信息是由设备发起的，因此这里直接返回
    server_sample_rate_ = 16000;
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}