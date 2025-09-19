#ifndef GIZWITS_C2_6824_LIGHT_ROBOT_STRIP_LED_H_
#define GIZWITS_C2_6824_LIGHT_ROBOT_STRIP_LED_H_

#include <stdint.h>
#include <driver/gpio.h>
#include <led_strip.h>
#include "config.h"

class StripLed {
public:
    // 默认使用 LED_GPIO 和 4 颗灯
    explicit StripLed(gpio_num_t gpio = LED_GPIO, uint8_t led_count = 4);
    ~StripLed();

    // 0-100 亮度
    void SetBrightness(uint8_t brightness);
    // 0-255 颜色，按当前亮度输出
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
    void TurnOff();
    void StartBreathing(uint8_t r = 0, uint8_t g = 255, uint8_t b = 0);
    void StopBreathing();

private:
    void Apply();

    led_strip_handle_t led_strip_ = nullptr;
    uint8_t led_count_ = 0;

    uint8_t base_r_ = 0;
    uint8_t base_g_ = 0;
    uint8_t base_b_ = 0;
    uint8_t brightness_ = 32; // 0-100

    // 呼吸效果相关
    bool breathing_ = false;
    esp_timer_handle_t breathing_timer_ = nullptr;
    uint8_t breathing_step_ = 0;
    bool breathing_up_ = true;
    uint8_t breathing_r_ = 0;
    uint8_t breathing_g_ = 255;
    uint8_t breathing_b_ = 0;
};

#endif // GIZWITS_C2_6824_LIGHT_ROBOT_STRIP_LED_H_


