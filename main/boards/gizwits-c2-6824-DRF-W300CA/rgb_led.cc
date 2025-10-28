#include "rgb_led.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "RgbLed";

RgbLed::RgbLed() {
}

RgbLed::~RgbLed() {
    StopBreathing();
}

void RgbLed::Initialize() {
    if (initialized_) return;
    
    // 配置 LEDC 定时器
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,  // 10位分辨率 (1024级)
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 15000,  // 30kHz，完全无噪音
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_config);

    // 配置红色 LED 通道
    ledc_channel_config_t red_channel = {
        .gpio_num = RGB_LED_R_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_3,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&red_channel);

    // 配置绿色 LED 通道
    ledc_channel_config_t green_channel = {
        .gpio_num = RGB_LED_G_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_4,  // 修正为通道4
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&green_channel);

    // 配置蓝色 LED 通道
    ledc_channel_config_t blue_channel = {
        .gpio_num = RGB_LED_B_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_5,  // 修正为通道5
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&blue_channel);

    initialized_ = true;
    
    // 初始化后确保LED关闭
    red_ = 0;
    green_ = 0;
    blue_ = 0;
    brightness_ = 0;
    UpdatePwm();
    
    ESP_LOGI(TAG, "RGB LED initialized on GPIOs: R=%d, G=%d, B=%d", 
             RGB_LED_R_GPIO, RGB_LED_G_GPIO, RGB_LED_B_GPIO);
}

void RgbLed::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!initialized_) return;
    
    red_ = r;
    green_ = g;
    blue_ = b;
    UpdatePwm();
}

void RgbLed::SetBrightness(uint8_t brightness) {
    if (!initialized_) return;
    
    brightness_ = brightness > 100 ? 100 : brightness;
    UpdatePwm();
}

void RgbLed::UpdatePwm() {
    if (!initialized_) return;
    
    // 应用亮度系数 - 10位分辨率 (0-1023)
    uint32_t scaled_r = (red_ * brightness_ * 1023) / (100 * 255);
    uint32_t scaled_g = (green_ * brightness_ * 1023) / (100 * 255);
    uint32_t scaled_b = (blue_ * brightness_ * 1023) / (100 * 255);

    // 设置 PWM 占空比 (交换G和B通道映射)
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3, scaled_r);  // 红色
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, scaled_g);  // 绿色 -> 通道5
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, scaled_b);  // 蓝色 -> 通道4

    // 更新 PWM 输出
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_3);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);  // 绿色
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4);  // 蓝色
    
}

void RgbLed::StartBreathing(uint8_t r, uint8_t g, uint8_t b) {
    if (!initialized_) return;
    
    breathing_ = true;
    red_ = r;
    green_ = g;
    blue_ = b;
    
    // 创建呼吸效果定时器
    esp_timer_create_args_t timer_args = {
        .callback = BreathingCallback,
        .arg = this,
        .name = "rgb_breathing"
    };
    esp_timer_create(&timer_args, &breathing_timer_);
    esp_timer_start_periodic(breathing_timer_, 50000); // 50ms
}

void RgbLed::StopBreathing() {
    if (breathing_timer_) {
        esp_timer_stop(breathing_timer_);
        esp_timer_delete(breathing_timer_);
        breathing_timer_ = nullptr;
    }
    breathing_ = false;
}

void RgbLed::TurnOff() {
    SetColor(0, 0, 0);
}

void RgbLed::BreathingCallback(void* arg) {
    RgbLed* rgb_led = static_cast<RgbLed*>(arg);
    if (!rgb_led->breathing_) return;
    
    static uint8_t brightness = 0;
    static bool increasing = true;
    
    if (increasing) {
        brightness += 5;
        if (brightness >= 100) {
            brightness = 100;
            increasing = false;
        }
    } else {
        brightness -= 5;
        if (brightness <= 0) {
            brightness = 0;
            increasing = true;
        }
    }
    
    rgb_led->SetBrightness(brightness);
}
