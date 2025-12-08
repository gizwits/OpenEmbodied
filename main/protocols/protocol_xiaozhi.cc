#include "protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(AudioStreamPacket&& packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}


void Protocol::UpdateRoomParams(const RoomParams& params) {
}

void Protocol::OnAudioChannelClosed(std::function<void(bool is_clean)> callback) {
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
    // 记录打断AI说话的时间戳
    abort_speaking_timestamp_ = std::chrono::steady_clock::now();
    abort_speaking_recorded_ = true;
    ESP_LOGI(TAG, "Abort speaking timestamp recorded, will ignore server audio for 1s");
    
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    ESP_LOGI(TAG, "SendAbortSpeaking: %s", message.c_str());
    SendText(message);
}


void Protocol::SendMessage(const std::string& message) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
    "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + message + "\"}";
    SendText(json);
}

void Protocol::SendTextToAI(const std::string& message) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
    "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + message + "\"}";
    SendText(json);
}

void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string json = "{\"session_id\":\"" + session_id_ + 
                      "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    SendText(json);
}

void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}


bool Protocol::IsAudioChannelBusy() const {
    return busy_sending_audio_;
}

void Protocol::PreAbortSpeaking() {
    SendAbortSpeaking(kAbortReasonNone);
}


void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}

void Protocol::SetAudioUploadEnabled(bool enabled) {
    busy_sending_audio_ = !enabled;  // busy_sending_audio_ = true 表示禁用上传
    ESP_LOGI(TAG, "Audio upload %s", enabled ? "enabled" : "disabled");
}

void Protocol::GenerateTTSFromText(const std::string& text) {
    // 默认空实现，子类可以重写
}
