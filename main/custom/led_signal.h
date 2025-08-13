#ifndef LED_SIGNAL_H
#define LED_SIGNAL_H

#include "led/gpio_led.h"
#include "led/circular_strip.h"
#include "application.h"

class LedSignal {

public:
    LedSignal(gpio_num_t red_gpio, ledc_channel_t red_channel, gpio_num_t green_gpio, ledc_channel_t green_channel, gpio_num_t blue_gpio, ledc_channel_t blue_channel);
    ~LedSignal(); // 添加析构函数声明

    void SetColor(uint8_t red, uint8_t green, uint8_t blue);
    void SetBrightness(uint8_t brightness);

    void CycleColorsWithFade(uint32_t interval_ms, uint8_t max_brightness);
    void MonitorAndUpdateLedState();


private:
    void InitializeLeds();
    GpioLed* red_led_;
    GpioLed* green_led_;
    GpioLed* blue_led_;
    bool CheckIfWorking(); // 检查设备是否处于工作状态
    bool CheckIfCharging(); // 检查设备是否正在充电
    bool CheckIfBatteryLow(); // 检查设备电量是否低
};

#endif