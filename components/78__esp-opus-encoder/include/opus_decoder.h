#ifndef _OPUS_DECODER_WRAPPER_H_
#define _OPUS_DECODER_WRAPPER_H_

#include <functional>
#include <vector>
#include <cstdint>
#include <mutex>

#include "opus.h"


class OpusDecoderWrapper {
public:
    OpusDecoderWrapper(int sample_rate, int channels, int duration_ms = 60);
    ~OpusDecoderWrapper();

    bool Decode(std::vector<uint8_t>&& opus, std::vector<int16_t>& pcm);
    void ResetState();
    void Config(int sample_rate, int channels, int duration_ms);

    inline int sample_rate() const {
        return sample_rate_;
    }

    inline int duration_ms() const {
        return duration_ms_;
    }

private:
    std::mutex mutex_;
    struct OpusDecoder* audio_dec_ = nullptr;
    int frame_size_;
    int sample_rate_;
    int duration_ms_;
};

#endif // _OPUS_DECODER_WRAPPER_H_
