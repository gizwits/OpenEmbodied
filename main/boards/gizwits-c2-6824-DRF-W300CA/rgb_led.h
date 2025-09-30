#ifndef __RGB_LED_H__
#define __RGB_LED_H__

#include <driver/ledc.h>
#include <esp_timer.h>
#include "config.h"

class RgbLed {
private:
    bool initialized_ = false;
    uint8_t red_ = 0;
    uint8_t green_ = 0;
    uint8_t blue_ = 0;
    uint8_t brightness_ = 100;
    bool breathing_ = false;
    esp_timer_handle_t breathing_timer_ = nullptr;

    void UpdatePwm();
    static void BreathingCallback(void* arg);

public:
    RgbLed();
    ~RgbLed();

    void Initialize();
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
    void SetBrightness(uint8_t brightness);
    void StartBreathing(uint8_t r = 255, uint8_t g = 0, uint8_t b = 0);
    void StopBreathing();
    void TurnOff();
    bool IsInitialized() const { return initialized_; }
};

#endif // __RGB_LED_H__
