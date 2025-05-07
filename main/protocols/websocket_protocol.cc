#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "mbedtls/base64.h"
#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"
#include "protocols/mcp.h"

#define TAG "WS"

// Received message on topic: llm/nd7ec83a/config/response
// in_str:{"method":"websocket.auth.response","body":{"platform_type":1,"token_quota":500000,"coze_websocket":{"bot_id":"7483788991729270847","voice_id":"7426720361753968677","user_id":"nd7ec83a","conv_id":"7486307379559104521","access_token":"czs_qNqGYuaxk7GQXXz5l6RwjaUYyE6y0sqCuRUl1enbJUPkMYQWgosyTdLpCDOZcEZOr","expires_in":3540}}}

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    if (websocket_ != nullptr) {
        delete websocket_;
    }
    vEventGroupDelete(event_group_handle_);
}

void WebsocketProtocol::Start() {
}

void WebsocketProtocol::SendAudio(const std::vector<uint8_t>& data) {
    if (websocket_ == nullptr || !websocket_->IsConnected() || data.empty()) {
        return;
    }
    // ESP_LOGI(TAG, "Send audio data size: %d", data.size());
    size_t data_size = data.size() * sizeof(uint8_t);
    size_t out_len = 4 * ((data_size + 2) / 3);  // base64 编码后的长度
    char *base64_buffer = (char*)malloc(out_len + 1);
    if (!base64_buffer) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        return;
    }

    size_t encoded_len;
    mbedtls_base64_encode((unsigned char *)base64_buffer, out_len + 1, &encoded_len,
                         (const unsigned char*)data.data(), data_size);
    base64_buffer[encoded_len] = '\0';

    // 创建事件 ID (使用随机数，确保为正数)
    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);

    // 构建消息
    std::string message = "{";
    message += "\"id\":\"" + std::string(event_id) + "\",";
    message += "\"event_type\":\"input_audio_buffer.append\",";
    message += "\"data\":{";
    message += "\"delta\":\"" + std::string(base64_buffer) + "\"";
    message += "}";
    message += "}";

    // 发送消息
    websocket_->Send(message);
    
    // 清理
    free(base64_buffer);
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
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
        websocket_ = nullptr;
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (websocket_ != nullptr) {
        delete websocket_;
    }
    if (room_params_.bot_id.empty() || room_params_.access_token.empty() || room_params_.voice_id.empty()) {
        ESP_LOGE(TAG, "Bot ID or access token or voice id is empty");
        return false;
    }

    error_occurred_ = false;
    std::string url = std::string("ws://") + std::string(room_params_.api_domain) + std::string("/v1/chat") + std::string("?bot_id=") + std::string(room_params_.bot_id);
    std::string token = "Bearer " + std::string(room_params_.access_token);

    message_cache_ = "";
    websocket_ = Board::GetInstance().CreateWebSocket();
    websocket_->SetHeader("Authorization", token.c_str());
    websocket_->SetHeader("x-tt-env", "ppe_uplink_opus");
    websocket_->SetHeader("x-use-ppe", "1");

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        // ESP_LOGI(TAG, "Received data: %s", data);
        if (!data || len == 0) {
            return;
        }
        std::string_view str_data(data, len);  // 将 const char* 转换为 string_view

        constexpr std::string_view key = "\"event_type\":\"";
        size_t event_start = str_data.find(key);
        if (event_start == std::string_view::npos) {
            return;
        }

        event_start += key.length();

        // 找到事件类型结束的引号位置
        size_t event_end = str_data.find('"', event_start);
        if (event_end == std::string_view::npos) {
            return;
        }

        // 提取事件类型
        std::string_view event_type = str_data.substr(event_start, event_end - event_start);

        if (event_type.empty() || event_type.length() >= 64) {
            return;
        }

        if(event_type == "conversation.audio.delta") {
            // 查找 content 字段
            constexpr std::string_view content_key = "\"content\":\"";
            size_t content_start = str_data.find(content_key);
            if (content_start != std::string_view::npos) {
                content_start += content_key.length();
                
                // 找到 content 结束的位置 (下一个未转义的引号)
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
                    // 提取 base64 编码的内容
                    std::string_view base64_content = str_data.substr(content_start, content_end - content_start);
                    
                    // 计算解码后的长度
                    size_t output_len = 0;
                    mbedtls_base64_decode(nullptr, 0, &output_len, 
                        (const unsigned char*)base64_content.data(), 
                        base64_content.length());

                    // 分配内存并解码
                    std::vector<uint8_t> decoded_data(output_len);
                    size_t actual_len = 0;
                    int ret = mbedtls_base64_decode(
                        decoded_data.data(), decoded_data.size(), &actual_len,
                        (const unsigned char*)base64_content.data(), 
                        base64_content.length());

                    if (ret == 0 && actual_len > 0) {
                        // 回调处理解码后的音频数据
                        if (on_incoming_audio_ != nullptr) {
                            on_incoming_audio_(std::move(decoded_data));
                        }
                    }
                }
            }
        } else {
            auto root = cJSON_Parse(data);
            if (event_type == "chat.created") {
                ParseServerHello(root);
            } else if (event_type == "conversation.audio_transcript.update") {
                // 识别到的文本
                auto data_json = cJSON_GetObjectItem(root, "data");
                auto content_json = cJSON_GetObjectItem(data_json, "content");
                std::string message = "{";
                message += "\"type\":\"stt\",";
                message += "\"text\":\"" + std::string(content_json->valuestring) + "\"";
                message += "}";
                 auto message_json = cJSON_Parse(message.c_str());
                on_incoming_json_(message_json);
                cJSON_Delete(message_json);

            } else if (event_type == "conversation.chat.in_progress") {
                // 清空字幕缓存
                message_cache_ = "";
                std::string message = "{";
                message += "\"type\":\"tts\",";
                message += "\"state\":\"start\"";
                message += "}";
                auto message_json = cJSON_Parse(message.c_str());
                on_incoming_json_(message_json);
                cJSON_Delete(message_json);
            } else if (event_type == "conversation.audio.completed") {
                std::string message = "{";
                message += "\"type\":\"tts\",";
                message += "\"state\":\"stop\"";
                message += "}";
                auto message_json = cJSON_Parse(message.c_str());
                on_incoming_json_(message_json);
                cJSON_Delete(message_json);
            } else if (event_type == "conversation.message.delta") {
                // 解析 content
                auto data_json = cJSON_GetObjectItem(root, "data");
                auto content_json = cJSON_GetObjectItem(data_json, "content");

                message_cache_ += std::string(content_json->valuestring);
                std::string message = "{";
                message += "\"type\":\"tts\",";
                message += "\"state\":\"sentence_start\",";
                message += "\"text\":\"" + message_cache_ + "\"";
                message += "}";
                auto message_json = cJSON_Parse(message.c_str());
                on_incoming_json_(message_json);
                cJSON_Delete(message_json);
            } else if (event_type == "conversation.chat.requires_action") {
                CozeMCPParser::getInstance().handle_mcp(str_data);
            }
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server");
        SetError(Lang::Strings::SERVER_NOT_FOUND);
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

    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);
    
    std::string codec = "opus";
    std::string message = "{";
    message += "\"id\":\"" + std::string(event_id) + "\",";
    message += "\"event_type\":\"chat.update\",";
    message += "\"data\":{";
    message += "\"turn_detection\": {";
    message += "\"type\": \"server_vad\",";  // 判停类型，client_vad/server_vad，默认为 client_vad
    message += "\"prefix_padding_ms\": 300,"; // server_vad模式下，VAD 检测到语音之前要包含的音频量，单位为 ms。默认为 600ms
    message += "\"silence_duration_ms\": 300"; // server_vad模式下，检测语音停止的静音持续时间，单位为 ms。默认为 800ms
    message += "},";
    message += "\"chat_config\":{";
    message += "\"auto_save_history\":true,";
    message += "\"conversation_id\":\"" + room_params_.conv_id + "\",";
    message += "\"user_id\":\"" + room_params_.user_id + "\",";
    message += "\"meta_data\":{},";
    message += "\"custom_variables\":{},";
    message += "\"extra_params\":{}";
    message += "},";
    message += "\"input_audio\":{";
    message += "\"format\":\"pcm\",";
    message += "\"codec\":\"opus\",";
    message += "\"sample_rate\":16000,";
    message += "\"channel\":1,";
    message += "\"bit_depth\":16";
    message += "},";
    message += "\"asr_config\":{\"user_language\":\"" + std::string(room_params_.voice_lang) + "\"},";
    message += "\"output_audio\":{";
    message += "\"codec\":\"" + codec + "\",";
    message += "\"opus_config\":{";
    message += "\"sample_rate\":16000,";
    message += "\"use_cbr\":false,";
    message += "\"frame_size_ms\":60,";
    message += "\"limit_config\":{";
    message += "\"period\":1,";
    message += "\"max_frame_num\":18";
    message += "}";
    message += "},";
    message += "\"speech_rate\":0,";
    message += "\"voice_id\":\"" + std::string(room_params_.voice_id) + "\"";
    message += "}";
    message += "}";
    message += "}";
    
    websocket_->Send(message);

    ESP_LOGI(TAG, "Send message: %s", message.c_str());

    return true;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    // COZE 的音频信息是由设备发起的，因此这里直接返回
    server_sample_rate_ = 16000;
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}