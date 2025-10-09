#ifndef _VB6824_AUDIO_CODEC_H
#define _VB6824_AUDIO_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>
#include <esp_timer.h>
#include "vb6824.h"
#include <functional>

#include "freertos/timers.h"

class VbAduioCodec : public AudioCodec {
private:
    void ready();
    void WakeUp(std::string command);
    virtual int Read(int16_t* dest, int samples) override;
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    virtual int Read(uint8_t* dest, int samples) override;
#endif
    virtual int Write(const int16_t* data, int samples) override;
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
    virtual int Write(uint8_t* opus, int samples) override;
#endif
    std::function<void(std::string)> on_wake_up_;
    bool frist_volume_is_set = false;
    void OtaEvent(vb6824_evt_t event_id, uint32_t data);
    std::string GenDevCode();

public:
    VbAduioCodec(gpio_num_t tx, gpio_num_t rx);
    void OnWakeUp(std::function<void(std::string)> callback);
    void SetOutputVolume(int volume) override;
    virtual void Start();
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    virtual bool InputData(std::vector<uint8_t>& opus) override;
#endif
    virtual void EnableInput(bool enable) override; 
    virtual void EnableOutput(bool enable) override; 
    int OtaStart(uint8_t mode=0);
    bool InOtaMode(bool reShowIfInOta);
    enum{
        OTA_ERR_NOT_CONNECTED = 0,
        OTA_ERR_NOT_SUPPORT = 1,
        OTA_ERR_IN_OTA_MODE = 2,
        OTA_ERR_OTHER = 3,
        OTA_OK = 4
    };
};

#endif