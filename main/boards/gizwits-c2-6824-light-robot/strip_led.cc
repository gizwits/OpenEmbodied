#include "strip_led.h"

#include <algorithm>
#include <esp_check.h>
#include <led_strip_spi.h>
#include <esp_timer.h>

StripLed::StripLed(gpio_num_t gpio, uint8_t led_count) : led_count_(led_count) {
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = led_count_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.flags.invert_out = false;

    // 使用 SPI 后端（ESP32-C2 无 RMT）
    led_strip_spi_config_t spi_config = {};
    spi_config.clk_src = SPI_CLK_SRC_DEFAULT;
    spi_config.spi_bus = SPI2_HOST; // C2 支持 SPI2_HOST
    spi_config.flags.with_dma = 0;

    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip_));
    led_strip_clear(led_strip_);
}

StripLed::~StripLed() {
    StopBreathing();
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

void StripLed::StartBreathing(uint8_t r, uint8_t g, uint8_t b) {
    StopBreathing(); // 先停止之前的呼吸效果
    
    breathing_r_ = r;
    breathing_g_ = g;
    breathing_b_ = b;
    breathing_step_ = 0;
    breathing_up_ = true;
    breathing_ = true;
    
    // 创建定时器
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<StripLed*>(arg);
            if (!self->breathing_) return;
            
            // 呼吸效果：0-100 循环（加快速度）
            if (self->breathing_up_) {
                self->breathing_step_ += 5;  // 从2改为5，加快上升速度
                if (self->breathing_step_ >= 100) {
                    self->breathing_step_ = 100;
                    self->breathing_up_ = false;
                }
            } else {
                self->breathing_step_ -= 5;  // 从2改为5，加快下降速度
                if (self->breathing_step_ <= 0) {
                    self->breathing_step_ = 0;
                    self->breathing_up_ = true;
                }
            }
            
            // 应用呼吸亮度
            uint16_t scale = (self->breathing_step_ * 255) / 100;
            uint8_t br = (self->breathing_r_ * scale) / 255;
            uint8_t bg = (self->breathing_g_ * scale) / 255;
            uint8_t bb = (self->breathing_b_ * scale) / 255;
            
            for (int i = 0; i < self->led_count_; ++i) {
                led_strip_set_pixel(self->led_strip_, i, br, bg, bb);
            }
            led_strip_refresh(self->led_strip_);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "breathing_timer",
        .skip_unhandled_events = false,
    };
    
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &breathing_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(breathing_timer_, 30 * 1000)); // 30ms 更新一次，更快
}

void StripLed::StopBreathing() {
    if (breathing_timer_) {
        esp_timer_stop(breathing_timer_);
        esp_timer_delete(breathing_timer_);
        breathing_timer_ = nullptr;
    }
    breathing_ = false;
}


