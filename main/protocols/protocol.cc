#include "protocol.h"

#include <esp_log.h>
#include <string.h>

#define TAG "Protocol"
#include <esp_random.h>

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(AudioStreamPacket&& packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendAbortSpeaking(AbortReason reason) {

    char event_id[32];
    uint32_t random_value = esp_random();
    snprintf(event_id, sizeof(event_id), "%lu", random_value);
    
    std::string message = "{\"id\":\"" + std::string(event_id) + "\",\"event_type\":\"conversation.chat.cancel\"}";

    SendText(message);
}

void Protocol::SendMessage(const std::string& message) {
    const char *init_message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"%s\""
        "}"
    "}";
    
    char *init_message_str = (char *)malloc(strlen(init_message) + message.length() + 1);
    snprintf(init_message_str, strlen(init_message) + message.length() + 1, init_message, message.c_str());
    SendText(init_message_str);
    free(init_message_str);
}


void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    const char *init_message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"你好\""
        "}"
    "}";
    SendText(init_message);
}


void Protocol::SendStartListening(ListeningMode mode) {
    // std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    // message += ",\"type\":\"listen\",\"state\":\"start\"";
    // if (mode == kListeningModeRealtime) {
    //     message += ",\"mode\":\"realtime\"";
    // } else if (mode == kListeningModeAutoStop) {
    //     message += ",\"mode\":\"auto\"";
    // } else {
    //     message += ",\"mode\":\"manual\"";
    // }
    // message += "}";
    // SendText(message);
}

void Protocol::SendStopListening() {
    // std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    // SendText(message);
}

void Protocol::SendIotDescriptors(const std::string& descriptors) {
    // cJSON* root = cJSON_Parse(descriptors.c_str());
    // if (root == nullptr) {
    //     ESP_LOGE(TAG, "Failed to parse IoT descriptors: %s", descriptors.c_str());
    //     return;
    // }

    // if (!cJSON_IsArray(root)) {
    //     ESP_LOGE(TAG, "IoT descriptors should be an array");
    //     cJSON_Delete(root);
    //     return;
    // }

    // int arraySize = cJSON_GetArraySize(root);
    // for (int i = 0; i < arraySize; ++i) {
    //     cJSON* descriptor = cJSON_GetArrayItem(root, i);
    //     if (descriptor == nullptr) {
    //         ESP_LOGE(TAG, "Failed to get IoT descriptor at index %d", i);
    //         continue;
    //     }

    //     cJSON* messageRoot = cJSON_CreateObject();
    //     cJSON_AddStringToObject(messageRoot, "session_id", session_id_.c_str());
    //     cJSON_AddStringToObject(messageRoot, "type", "iot");
    //     cJSON_AddBoolToObject(messageRoot, "update", true);

    //     cJSON* descriptorArray = cJSON_CreateArray();
    //     cJSON_AddItemToArray(descriptorArray, cJSON_Duplicate(descriptor, 1));
    //     cJSON_AddItemToObject(messageRoot, "descriptors", descriptorArray);

    //     char* message = cJSON_PrintUnformatted(messageRoot);
    //     if (message == nullptr) {
    //         ESP_LOGE(TAG, "Failed to print JSON message for IoT descriptor at index %d", i);
    //         cJSON_Delete(messageRoot);
    //         continue;
    //     }

    //     SendText(std::string(message));
    //     cJSON_free(message);
    //     cJSON_Delete(messageRoot);
    // }

    // cJSON_Delete(root);
}

void Protocol::SendIotStates(const std::string& states) {
    // std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"iot\",\"update\":true,\"states\":" + states + "}";
    // SendText(message);
}

// void Protocol::SendMcpMessage(const std::string& payload) {
//     std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
//     SendText(message);
// }

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %lld seconds", duration.count());
    }
    return timeout;
}

bool Protocol::IsAudioChannelBusy() const {
    return busy_sending_audio_;
}


void Protocol::UpdateRoomParams(const RoomParams& params) {
    ESP_LOGI(TAG, "Updating WebSocket parameters:");
    ESP_LOGI(TAG, "  bot_id: %s", params.bot_id.c_str());
    ESP_LOGI(TAG, "  voice_id: %s", params.voice_id.c_str());
    ESP_LOGI(TAG, "  conv_id: %s", params.conv_id.c_str());
    ESP_LOGI(TAG, "  access_token: %s", params.access_token.c_str());

    // 保存
    room_params_ = params;
}
