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

    // need_abort_speaking_ = true;
    // // 如果已有定时器在运行，先停止它
    // ESP_LOGI(TAG, "PreAbortSpeaking: busy_timer_ != nullptr");
    // if (busy_timer_ != nullptr) {
    //     esp_timer_stop(busy_timer_);
    //     esp_timer_delete(busy_timer_);
    //     busy_timer_ = nullptr;
    // }
    // ESP_LOGI(TAG, "PreAbortSpeaking: busy_timer_ == nullptr");
    // // 创建300ms定时器
    // esp_timer_create_args_t timer_args = {
    //     .callback = [](void* arg) {
    //         auto* self = static_cast<Protocol*>(arg);
    //         self->need_abort_speaking_ = false;
    //         self->SendAbortSpeaking(kAbortReasonNone);
    //     },
    //     .arg = this,
    //     .dispatch_method = ESP_TIMER_TASK,
    //     .name = "BusyTimer",
    //     .skip_unhandled_events = false,
    // };

    // ESP_LOGI(TAG, "PreAbortSpeaking: esp_timer_create");
    // esp_err_t ret = esp_timer_create(&timer_args, &busy_timer_);
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to create busy timer: %s", esp_err_to_name(ret));
    //     need_abort_speaking_ = false;
    //     return;
    // }

    // ret = esp_timer_start_once(busy_timer_, 200000); 
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to start busy timer: %s", esp_err_to_name(ret));
    //     esp_timer_delete(busy_timer_);
    //     busy_timer_ = nullptr;
    //     need_abort_speaking_ = false;
    //     return;
    // }
}


void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
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
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
