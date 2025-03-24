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

#define TAG "WS"

#define BOT_ID "7481549254856114230"
#define VOICE_ID "7426720361733013513"
#define CONFIG_WEBSOCKET_ACCESS_TOKEN "pat_RzK1r9uuCdUyB5o7PyU786AmYNHPsn68GNWuOkYlfHEgY2SruD2QRRwlRXnImRfU"

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
    
    // 计算 base64 编码后的长度
    size_t out_len = 4 * ((data.size() + 2) / 3);  // base64 编码后的长度
    char *base64_buffer = (char*)malloc(out_len + 1);  // +1 for null terminator
    if (!base64_buffer) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        return;
    }

    // 进行 base64 编码
    size_t encoded_len;
    mbedtls_base64_encode((unsigned char *)base64_buffer, out_len + 1, &encoded_len, 
                         data.data(), data.size());
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

    ESP_LOGI(TAG, "Send message: %s", message.c_str());
    // 发送消息
    websocket_->Send(message);
    
    // 清理
    free(base64_buffer);
}

void WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr) {
        return;
    }
    // TODO

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
    error_occurred_ = false;
    std::string url = CONFIG_COZE_WEBSOCKET_URL + std::string("?bot_id=") + std::string(BOT_ID);
    std::string token = "Bearer " + std::string(CONFIG_WEBSOCKET_ACCESS_TOKEN);
    websocket_ = Board::GetInstance().CreateWebSocket();
    websocket_->SetHeader("Authorization", token.c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        ESP_LOGI(TAG, "Received data: %s", data);
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
            // 需要做分包处理
        } else {
            auto root = cJSON_Parse(data);
            if (event_type == "chat.created") {
                ParseServerHello(root);
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
    
    std::string conversation_id = "7483457529922469928";  // You may want to manage this differently
    std::string user_id = "nb47c6f2";  // You may want to set this appropriately
    std::string codec = "opus";
    std::string message = "{";
    message += "\"id\":\"" + std::string(event_id) + "\",";
    message += "\"event_type\":\"chat.update\",";
    message += "\"data\":{";
    message += "\"chat_config\":{";
    message += "\"auto_save_history\":true,";
    message += "\"conversation_id\":\"" + conversation_id + "\",";
    message += "\"user_id\":\"" + user_id + "\",";
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
    message += "\"output_audio\":{";
    message += "\"codec\":\"" + codec + "\",";
    message += "\"opus_config\":{";
    message += "\"sample_rate\":16000,";
    message += "\"use_cbr\":false,";
    message += "\"frame_size_ms\":60,";
    message += "\"limit_config\":{";
    message += "\"period\":1,";
    message += "\"max_frame_num\":50";
    message += "}";
    message += "},";
    message += "\"speech_rate\":0,";
    message += "\"voice_id\":\"" + std::string(VOICE_ID) + "\"";
    message += "}";
    message += "}";
    message += "}";
    
    websocket_->Send(message);

    ESP_LOGI(TAG, "Send message: %s", message.c_str());

    return true;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    // COZE 的音频信息是由设备发起的，因此这里直接返回
    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
