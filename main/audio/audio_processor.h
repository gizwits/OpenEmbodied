#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms) = 0;
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    virtual void Feed(std::vector<uint8_t>&& opus) = 0;
#else
    virtual void Feed(std::vector<int16_t>&& data) = 0;
#endif

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    virtual void OnOutput(std::function<void(std::vector<uint8_t>&& opus)> callback) = 0;
#else
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
#endif
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;
};

#endif
