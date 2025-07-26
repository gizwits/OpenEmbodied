#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
void AudioCodec::OutputData(std::vector<uint8_t>& opus) {
    Write(opus.data(), opus.size());
}
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
bool AudioCodec::InputData(std::vector<uint8_t>& opus) {
    int samples = Read(opus.data(), opus.size());
    if (samples > 0) {
        return true;
    }
    return false;
}
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
bool AudioCodec::ConfigDecode(int sample_rate, int channels, int duration_ms){
    output_sample_rate_ = sample_rate;
    output_channels_ = channels;
    output_duration_ms_ = duration_ms;
    return true;
}
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
bool AudioCodec::ConfigEncode(int sample_rate, int channels, int duration_ms){
    input_sample_rate_ = sample_rate;
    input_channels_ = channels;
    input_duration_ms_ = duration_ms;
    return true;
}
#endif

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }
    
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
#else
#ifdef CONFIG_IDF_TARGET_ESP32C2
#else
    if(tx_handle_){
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }
#endif
#endif

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
#else
#ifdef CONFIG_IDF_TARGET_ESP32C2
#else
    if(rx_handle_){
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }
#endif
#endif

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
