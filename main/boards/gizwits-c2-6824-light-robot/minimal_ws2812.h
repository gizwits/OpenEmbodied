#ifndef MINIMAL_WS2812_H
#define MINIMAL_WS2812_H

#include <stdint.h>
#include <driver/gpio.h>
#include "config.h"

class MinimalWS2812 {
public:
    explicit MinimalWS2812(gpio_num_t gpio = LED_GPIO, uint8_t led_count = 4);
    ~MinimalWS2812();

    void SetColor(uint8_t r, uint8_t g, uint8_t b);
    void TurnOff();
    void SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

private:
    void SendBit(bool bit);
    void SendByte(uint8_t byte);
    void SendReset();
    
    gpio_num_t gpio_;
    uint8_t led_count_;
    uint8_t* pixel_data_;  // 存储像素数据
};

#endif // MINIMAL_WS2812_H
