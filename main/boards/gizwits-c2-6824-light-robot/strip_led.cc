#include "strip_led.h"

#include <algorithm>
#include <esp_check.h>

StripLed::StripLed(gpio_num_t gpio, uint8_t led_count) : led_count_(led_count) {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = led_count_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    // 使用通用 RMT 创建函数（与你当前头文件一致）
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);
}

StripLed::~StripLed() {
    if (led_strip_) {
        led_strip_del(led_strip_);
        led_strip_ = nullptr;
    }
}

void StripLed::SetBrightness(uint8_t brightness) {
    brightness_ = std::min<uint8_t>(brightness, 100);
    Apply();
}

void StripLed::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    base_r_ = r;
    base_g_ = g;
    base_b_ = b;
    Apply();
}

void StripLed::TurnOff() {
    if (!led_strip_) return;
    led_strip_clear(led_strip_);
}

void StripLed::Apply() {
    if (!led_strip_) return;
    // 将 0-100 的亮度映射成 0-255 的比例
    uint16_t scale = brightness_ == 100 ? 255 : (brightness_ * 255) / 100;
    uint8_t r = (base_r_ * scale) / 255;
    uint8_t g = (base_g_ * scale) / 255;
    uint8_t b = (base_b_ * scale) / 255;

    for (int i = 0; i < led_count_; ++i) {
        led_strip_set_pixel(led_strip_, i, r, g, b);
    }
    led_strip_refresh(led_strip_);
}


