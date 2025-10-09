#include "no_audio_processor.h"
#include <esp_log.h>

#define TAG "NoAudioProcessor"

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
}

#ifdef CONFIG_USE_EYE_STYLE_VB6824
void NoAudioProcessor::Feed(std::vector<uint8_t>&& opus) {
    if (!is_running_ || !output_callback_) {
        ESP_LOGE(TAG, "Feed called but not running or no callback, is_running_: %d", is_running_);
        return;
    }

    // VB6824 每次返回 40 字节的 OPUS 数据
    if (opus.size() != 40) {
        ESP_LOGE(TAG, "Feed opus size is not 40 bytes, feed size: %u", opus.size());
        return;
    }
    ESP_LOGD(TAG, "NoAudioProcessor feed: opus.size()=%zu", opus.size());
    output_callback_(std::move(opus));
}
#else
void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

    if (data.size() != frame_samples_) {
        ESP_LOGE(TAG, "Feed data size is not equal to frame size, feed size: %u, frame size: %u", data.size(), frame_samples_);
        return;
    }

    if (codec_->input_channels() == 2) {
        // If input channels is 2, we need to fetch the left channel data
        auto mono_data = std::vector<int16_t>(data.size() / 2);
        for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
            mono_data[i] = data[j];
        }
        output_callback_(std::move(mono_data));
    } else {
        output_callback_(std::move(data));
    }
}
#endif

void NoAudioProcessor::Start() {
    is_running_ = true;
}

void NoAudioProcessor::Stop() {
    is_running_ = false;
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

#ifdef CONFIG_USE_EYE_STYLE_VB6824
void NoAudioProcessor::OnOutput(std::function<void(std::vector<uint8_t>&& opus)> callback) {
    output_callback_ = callback;
}
#else
void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}
#endif

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    // 当使用 OPUS 编码时，VB6824 每次返回 40 字节的 OPUS 数据
    return 40;
#else
    return frame_samples_;
#endif
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
