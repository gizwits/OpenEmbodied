#include "led_signal.h"
#include <thread>
#include <chrono>
#include "esp_log.h"
#include "driver/ledc.h"
#include "power_manager.h"
#include <wifi_station.h>

#define TAG "LedSignal"

#define LEVEL_WORK_TIME_MIN 2
/*
红色：常亮代表充电中
蓝色：常亮代表处于工作状态，闪烁代表未配网或者网络不佳
绿色：充满电
优先级：优先显示工作状态，离开工作状态并且处于充电状态后 2 分钟才显示充电状态
*/


// Start of Selection

LedSignal::LedSignal(gpio_num_t red_gpio, ledc_channel_t red_channel, 
                    gpio_num_t green_gpio, ledc_channel_t green_channel, 
                    gpio_num_t blue_gpio, ledc_channel_t blue_channel) 
    : red_led_(new GpioLed(red_gpio, 1, LEDC_TIMER_0, red_channel)),
      green_led_(new GpioLed(green_gpio, 1, LEDC_TIMER_1, green_channel)),
      blue_led_(new GpioLed(blue_gpio, 1, LEDC_TIMER_2, blue_channel)) {
    InitializeLeds();
}

void LedSignal::SetColor(uint8_t red, uint8_t green, uint8_t blue) {
    auto setLedState = [](GpioLed* led, uint8_t brightness) {
        if (led) {
            if (brightness == 0) {
                led->TurnOff();
            } else {
                led->SetBrightness(brightness);
                led->TurnOn();
            }
        }
    };

    setLedState(red_led_, red);
    setLedState(green_led_, green);
    setLedState(blue_led_, blue);
}

void LedSignal::SetBrightness(uint8_t brightness) {
    brightness_ = brightness;
    if (red_led_) red_led_->SetBrightness(brightness);
    if (green_led_) green_led_->SetBrightness(brightness);
    if (blue_led_) blue_led_->SetBrightness(brightness);
}

uint8_t LedSignal::GetBrightness() const {
    return brightness_;
}

void LedSignal::InitializeLeds() {
    if (red_led_) {
        red_led_->SetBrightness(brightness_);
        red_led_->TurnOff();
    }
    if (green_led_) {
        green_led_->SetBrightness(brightness_);
        green_led_->TurnOff();
    }
    if (blue_led_) {
        blue_led_->SetBrightness(brightness_);
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
            brightness += fade_direction * 5; // 调整步长
            if (brightness >= max_brightness) {
                brightness = 0;
                current_color = (current_color + 1) % 3;
            }

            const char* color_names[] = {"Red", "Green", "Blue"};
            uint8_t red = 0, green = 0, blue = 0;

            switch (current_color) {
                case 0:
                    red = brightness;
                    break;
                case 1:
                    green = brightness;
                    break;
                case 2:
                    blue = brightness;
                    break;
            }

            auto now = std::chrono::steady_clock::now();
            auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            if (ms_since_epoch % 500 < 250) { // 每500ms闪烁一次
                SetColor(0, 0, 0); // 关闭所有LED
            } else {
                SetColor(red, green, blue);
            }

            ESP_LOGI(TAG, "Current color: %s, Brightness: %d", color_names[current_color], brightness);

            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }).detach();
}

void LedSignal::MonitorAndUpdateLedState() {
    std::thread([this]() {
        bool was_working = false;
        bool was_charging = false;
        bool was_fully_charged = false;
        auto last_non_working_time = std::chrono::steady_clock::now();

        while (true) {
            bool is_working = CheckIfWorking();
            bool is_charging = CheckIfCharging();
            bool is_battery_low = CheckIfBatteryLow();

            uint8_t red = 0, green = 0, blue = 0;
            uint8_t rgb_value = (brightness_ * 255) / 100; // 增加亮度权重变量，命名为rgb_value

            bool need_blink = false;

            if (is_working) {
                if (!was_working) {
                    was_working = true;
                }
                blue = rgb_value; // 蓝色代表处于工作状态
                last_non_working_time = std::chrono::steady_clock::now();
            } else {
                auto now = std::chrono::steady_clock::now();
                auto duration_since_non_working = std::chrono::duration_cast<std::chrono::minutes>(now - last_non_working_time).count();
                
                if (duration_since_non_working < LEVEL_WORK_TIME_MIN) {
                    blue = rgb_value; // 蓝色闪烁代表非工作状态
                    need_blink = true;
                } else {
                    if (is_battery_low) {
                        red = rgb_value; // 红色代表电量低
                        need_blink = true; // 低电量需要闪烁
                    } else if (is_charging) {
                        if (!was_charging) {
                            was_charging = true;
                        }
                        red = rgb_value; // 红色代表充电中
                    } else {
                        was_working = was_charging = was_fully_charged = false;
                        red = green = blue = 0; // 关闭所有LED
                    }
                }
            }

            if (need_blink) {
                auto now = std::chrono::steady_clock::now();
                auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                if (ms_since_epoch % 1000 < 500) { // 每1000ms闪烁一次
                    SetColor(0, 0, 0); // 关闭所有LED
                } else {
                    SetColor(red, green, blue);
                }
            } else {
                SetColor(red, green, blue);
            }
            
            // ESP_LOGI(TAG, "Current RGB values: R=%d, G=%d, B=%d, Brightness=%d", red, green, blue, brightness_);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

bool LedSignal::CheckIfWorking() {
    // 按wifi状态判断，如果ws连不上报非工作状态
    bool error_occurred = Application::GetInstance().HasWebsocketError();
    bool wifi_connected = WifiStation::GetInstance().IsConnected();
    // ESP_LOGI(TAG, "Websocket working: %s, WiFi connected: %s", error_occurred ? "false" : "true", wifi_connected ? "true" : "false");
    return !error_occurred && wifi_connected;
}

bool LedSignal::CheckIfCharging() {
    return PowerManager::GetInstance().IsCharging();
}

bool LedSignal::CheckIfBatteryLow() {
    return PowerManager::GetInstance().GetBatteryLevel() < 10;
}
