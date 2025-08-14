#ifndef LED_SIGNAL_H
#define LED_SIGNAL_H

#include "led/gpio_led.h"
#include "led/circular_strip.h"
#include "application.h"

class LedSignal {
public:
    static LedSignal& GetInstance() {
        static LedSignal instance; // 使用默认构造函数初始化对象
        return instance;
    }

    void SetColor(uint8_t red, uint8_t green, uint8_t blue);
    void SetBrightness(uint8_t brightness);
    uint8_t GetBrightness() const;
    void CycleColorsWithFade(uint32_t interval_ms, uint8_t max_brightness);
    void MonitorAndUpdateLedState();
    bool CheckIfWorking();
    bool CheckIfCharging();
    bool CheckIfBatteryLow();

private:

    LedSignal(gpio_num_t red_gpio = GPIO_NUM_2, ledc_channel_t red_channel = LEDC_CHANNEL_0, 
              gpio_num_t green_gpio = GPIO_NUM_4, ledc_channel_t green_channel = LEDC_CHANNEL_1, 
              gpio_num_t blue_gpio = GPIO_NUM_5, ledc_channel_t blue_channel = LEDC_CHANNEL_2);
    // led_signal_->CycleColorsWithFade(300, 100);
    ~LedSignal();
    void InitializeLeds();

    GpioLed* red_led_;
    GpioLed* green_led_;
    GpioLed* blue_led_;
    #define LED_DEFAULT_BRIGHTNESS 100 // Default brightness level
    uint8_t brightness_ = LED_DEFAULT_BRIGHTNESS; // Brightness level for the LED
};

#endif