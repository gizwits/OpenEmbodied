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

private:
    void Apply();

    led_strip_handle_t led_strip_ = nullptr;
    uint8_t led_count_ = 0;

    uint8_t base_r_ = 0;
    uint8_t base_g_ = 0;
    uint8_t base_b_ = 0;
    uint8_t brightness_ = 32; // 0-100
};

#endif // GIZWITS_C2_6824_LIGHT_ROBOT_STRIP_LED_H_


