#include "servo.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define SERVO_MIN_PULSEWIDTH_US (500)   // 优化后最小脉宽
#define SERVO_MAX_PULSEWIDTH_US (2500)  // 优化后最大脉宽
#define SERVO_MAX_DEGREE        (180)
#define SERVO_PWM_FREQ_HZ       (50)    // 50Hz
#define SERVO_PWM_RES_BITS      (13)

static const char* TAG = "Servo";

Servo::Servo(int gpio_num, int origin_angle)
    : gpio_num_(gpio_num), min_angle_(0), max_angle_(180), speed_dps_(60), duration_ms_(0), origin_angle_(origin_angle), running_(false), task_handle_(nullptr) {}

Servo::~Servo() {
    stop();
}

void Servo::begin() {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = SERVO_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = gpio_num_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel);
    set_angle(90); // 默认中位
}

void servo_task(void* arg) {
    Servo* servo = (Servo*)arg;
    int min_angle = servo->min_angle_;
    int max_angle = servo->max_angle_;
    int speed = servo->speed_dps_;
    int duration = servo->duration_ms_;
    int cur_angle = min_angle;
    int dir = 1;
    // 步进自适应，速度越大步进越大，最小1度
    int step = (speed >= 100) ? (speed / 100) : 1;
    if (step < 1) step = 1;
    int delay_per_deg = (speed > 0) ? (1000 / speed) : 20; // ms per度
    int elapsed = 0;
    while (servo->running_ && (duration == 0 || elapsed < duration)) {
        servo->set_angle(cur_angle);
        vTaskDelay(pdMS_TO_TICKS(delay_per_deg * step));
        elapsed += delay_per_deg * step;
        cur_angle += dir * step;
        if (cur_angle >= max_angle) {
            cur_angle = max_angle;
            dir = -1;
        } else if (cur_angle <= min_angle) {
            cur_angle = min_angle;
            dir = 1;
        }
    }
    // 运动结束后回到原点
    servo->set_angle(servo->origin_angle_);
    servo->running_ = false;
    vTaskDelete(nullptr);
}

void Servo::move(int min_angle, int max_angle, int speed_dps, int duration_ms) {
    stop();
    min_angle_ = min_angle;
    max_angle_ = max_angle;
    // 限制速度范围，防止损伤SG90
    if (speed_dps < 1) speed_dps = 1;
    if (speed_dps > 1000) speed_dps = 1000;
    speed_dps_ = speed_dps;
    duration_ms_ = duration_ms;
    running_ = true;
    xTaskCreate(servo_task, "servo_task", 2048, this, 5, (TaskHandle_t*)&task_handle_);
}

void Servo::stop() {
    if (running_) {
        running_ = false;
        if (task_handle_) {
            vTaskDelete((TaskHandle_t)task_handle_);
            task_handle_ = nullptr;
        }
    }
    // 停止时回到原点
    set_angle(origin_angle_);
}

void Servo::set_angle(int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t us = SERVO_MIN_PULSEWIDTH_US + (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) * angle / SERVO_MAX_DEGREE;
    uint32_t duty = (us * ((1 << SERVO_PWM_RES_BITS) - 1)) / (1000000 / SERVO_PWM_FREQ_HZ);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
