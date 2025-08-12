#include "led_signal.h"
#include <thread>
#include <chrono>
#include "esp_log.h"
#include "driver/ledc.h"

#define TAG "LedSignal"
/*
红色：常亮代表充电中
蓝色：常亮代表处于工作状态，闪烁代表未配网或者网络不佳
绿色：充满电
优先级：优先显示工作状态，离开工作状态并且处于充电状态后 2 分钟才显示充电状态
*/

LedSignal::LedSignal(gpio_num_t red_gpio, ledc_channel_t red_channel, 
                    gpio_num_t green_gpio, ledc_channel_t green_channel, 
                    gpio_num_t blue_gpio, ledc_channel_t blue_channel) {
    red_led_ = new GpioLed(red_gpio, 1, LEDC_TIMER_0,  red_channel);
    green_led_ = new GpioLed(green_gpio, 1, LEDC_TIMER_1, green_channel);
    blue_led_ = new GpioLed(blue_gpio, 1, LEDC_TIMER_2, blue_channel);
    InitializeLeds();
}

void LedSignal::SetColor(uint8_t red, uint8_t green, uint8_t blue) {
    if (red_led_) {
        if (red == 0) {
            red_led_->TurnOff();
        }
    }
    if (green_led_) {
        if (green == 0) {
            green_led_->TurnOff();
        }
    }
    if (blue_led_) {
        if (blue == 0) {
            blue_led_->TurnOff();
        }
    }

    if (red_led_) {
        if (red != 0) {
            red_led_->SetBrightness(red);
            red_led_->TurnOn();
        }
    }
    if (green_led_) {
        if (green != 0) {
            green_led_->SetBrightness(green);
            green_led_->TurnOn();
        }
    }
    if (blue_led_) {
        if (blue != 0) {
            blue_led_->SetBrightness(blue);
            blue_led_->TurnOn();
        }
    }
}

void LedSignal::SetBrightness(uint8_t brightness) {
    if (red_led_) red_led_->SetBrightness(brightness);
    if (green_led_) green_led_->SetBrightness(brightness);
    if (blue_led_) blue_led_->SetBrightness(brightness);
}

void LedSignal::InitializeLeds() {
    if (red_led_) {
        red_led_->SetBrightness(DEFAULT_BRIGHTNESS);
        red_led_->TurnOff();
    }
    if (green_led_) {
        green_led_->SetBrightness(DEFAULT_BRIGHTNESS);
        green_led_->TurnOff();
    }
    if (blue_led_) {
        blue_led_->SetBrightness(DEFAULT_BRIGHTNESS);
        blue_led_->TurnOff();
    }
}

LedSignal::~LedSignal() {
    delete red_led_;
    delete green_led_;
    delete blue_led_;
}

void LedSignal::CycleColorsWithFade(uint32_t interval_ms, uint8_t max_brightness) {

    if (red_led_ == nullptr && green_led_ == nullptr && blue_led_ == nullptr) {
        ESP_LOGE(TAG, "LedSignal: CycleColorsWithFade failed, leds not initialized");
        return;
    }

    if(max_brightness > 100) {
        max_brightness = 100;
    }

    std::thread([this, interval_ms, max_brightness]() {
        uint8_t current_color = 0;
        uint8_t brightness = 1;
        int8_t fade_direction = 1;

        while (true) {
            // Adjust brightness
            brightness += fade_direction * 5; // 调整步长
            if (brightness >= max_brightness) {
                brightness = 0;
                ESP_LOGI(TAG, "Current color: %d", current_color);
                current_color = (current_color + 1) % 3;
                ESP_LOGI(TAG, "Current color: %d", current_color);
            }

            // Set color based on current_color
            switch (current_color) {
                case 0:
                    SetColor(brightness, 0, 0); // Red
                    ESP_LOGI(TAG, "Current color: Red, Brightness: %d", brightness);
                    break;
                case 1:
                    SetColor(0, brightness, 0); // Green
                    ESP_LOGI(TAG, "Current color: Green, Brightness: %d", brightness);
                    break;
                case 2:
                    SetColor(0, 0, brightness); // Blue
                    ESP_LOGI(TAG, "Current color: Blue, Brightness: %d", brightness);
                    break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }).detach();
}
