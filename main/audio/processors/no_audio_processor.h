#ifndef DUMMY_AUDIO_PROCESSOR_H
#define DUMMY_AUDIO_PROCESSOR_H

#include <vector>
#include <functional>

#include "audio_processor.h"
#include "audio_codec.h"

class NoAudioProcessor : public AudioProcessor {
public:
    NoAudioProcessor() = default;
    ~NoAudioProcessor() = default;

    void Initialize(AudioCodec* codec, int frame_duration_ms) override;
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    void Feed(std::vector<uint8_t>&& opus) override;
#else
    void Feed(std::vector<int16_t>&& data) override;
#endif

    void Start() override;
    void Stop() override;
    bool IsRunning() override;
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    void OnOutput(std::function<void(std::vector<uint8_t>&& opus)> callback) override;
#else
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
#endif
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    std::function<void(std::vector<uint8_t>&& opus)> output_callback_;
#else
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
#endif
    std::function<void(bool speaking)> vad_state_change_callback_;
    bool is_running_ = false;
};

#endif 