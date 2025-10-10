#include "motor_control.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "MotorControl";

MotorControl::MotorControl() {
}

MotorControl::~MotorControl() {
    Stop();
}

void MotorControl::Initialize() {
    if (initialized_) return;
    
    // 配置 LEDC 定时器
    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,  // 10位分辨率，获得更精细控制
        .timer_num = MOTOR_LEDC_TIMER,
        .freq_hz = MOTOR_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_config);
    
    // 配置电机控制通道
    ledc_channel_config_t channel_config = {
        .gpio_num = MOTOR_CTRL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = MOTOR_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = MOTOR_LEDC_TIMER,
        .duty = 0,  // 初始关闭
        .hpoint = 0
    };
    ledc_channel_config(&channel_config);
    
    initialized_ = true;
    ESP_LOGI(TAG, "Motor control initialized on GPIO %d with PWM", MOTOR_CTRL_GPIO);
}

void MotorControl::Start() {
    if (!initialized_) return;
    
    if (!running_) {
        running_ = true;
        // 使用PWM控制电机速度
        uint32_t duty = (speed_ * 1023) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL);
        ESP_LOGI(TAG, "Motor started, speed: %d%%, duty: %lu, direction: %s", 
                 speed_, duty, forward_ ? "forward" : "reverse");
        ESP_LOGI(TAG, "PWM frequency: %lu Hz, duty cycle: %.1f%%", 
                 MOTOR_PWM_FREQ, (float)duty * 100.0f / 1023.0f);
    }
}

void MotorControl::Stop() {
    if (!initialized_) return;
    
    if (running_) {
        running_ = false;
        // 设置PWM duty为0关闭电机
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL);
        ESP_LOGI(TAG, "Motor stopped");
    }
}

void MotorControl::SetSpeed(uint8_t speed) {
    if (!initialized_) return;
    
    // 限制速度在0-100%
    speed_ = speed > 100 ? 100 : speed;
    ESP_LOGI(TAG, "Motor speed set to %d%%", speed_);
    
    // 如果电机正在运行，直接更新PWM duty
    if (running_) {
        uint32_t duty = (speed_ * 1023) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_LEDC_CHANNEL);
        ESP_LOGI(TAG, "Motor speed updated, duty: %lu (%.1f%%)", 
                 duty, (float)duty * 100.0f / 1023.0f);
    }
}

void MotorControl::SetDirection(bool forward) {
    if (!initialized_) return;
    
    forward_ = forward;
    ESP_LOGI(TAG, "Motor direction set to %s", forward_ ? "forward" : "reverse");
    
    // 如果电机正在运行，重新启动以应用新方向
    if (running_) {
        Stop();
        vTaskDelay(pdMS_TO_TICKS(10));
        Start();
    }
}