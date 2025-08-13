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

LedSignal::LedSignal(gpio_num_t red_gpio, ledc_channel_t red_channel, 
                    gpio_num_t green_gpio, ledc_channel_t green_channel, 
                    gpio_num_t blue_gpio, ledc_channel_t blue_channel) {
    red_led_ = new GpioLed(red_gpio, 1, LEDC_TIMER_0,  red_channel);
    green_led_ = new GpioLed(green_gpio, 1, LEDC_TIMER_1, green_channel);
    blue_led_ = new GpioLed(blue_gpio, 1, LEDC_TIMER_2, blue_channel);
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

// driver test unit
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
                current_color = (current_color + 1) % 3;
            }

            // Set color based on current_color
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

            // Implement blinking logic
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

            bool need_blink = false;

            if (is_working) {
                if (!was_working) {
                    // ESP_LOGI(TAG, "LED State: Working (Blue)");
                    was_working = true;
                }
                blue = 255; // 蓝色代表处于工作状态
                last_non_working_time = std::chrono::steady_clock::now();
            } else {
                auto now = std::chrono::steady_clock::now();
                auto duration_since_non_working = std::chrono::duration_cast<std::chrono::minutes>(now - last_non_working_time).count();
                
                if (duration_since_non_working < LEVEL_WORK_TIME_MIN) {
                    // ESP_LOGI(TAG, "LED State: Non-working (Blue Blinking)");
                    blue = 255; // 蓝色闪烁代表非工作状态
                    need_blink = true;
                } else {
                    if (is_battery_low) {
                        // ESP_LOGI(TAG, "LED State: Battery Low (Red Blinking)");
                        red = 255; // 红色代表电量低
                        need_blink = true; // 低电量需要闪烁
                    } else if (is_charging) {
                        if (!was_charging) {
                            // ESP_LOGI(TAG, "LED State: Charging (Red)");
                            was_charging = true;
                        }
                        red = 255; // 红色代表充电中
                    } else {
                        // ESP_LOGI(TAG, "LED State: Off");
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

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

bool LedSignal::CheckIfWorking() {

    return Application::GetInstance().IsWebsocketWorking() && WifiStation::GetInstance().IsConnected();
}

bool LedSignal::CheckIfCharging() {
    // 使用PowerManager检查设备是否正在充电
    return PowerManager::GetInstance().IsCharging();
}

bool LedSignal::CheckIfBatteryLow() {
    // 使用PowerManager检查设备是否充满电
    return PowerManager::GetInstance().GetBatteryLevel() < 10;
}
