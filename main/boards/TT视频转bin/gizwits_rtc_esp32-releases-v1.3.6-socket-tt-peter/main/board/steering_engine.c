#include "esp_err.h"
#include "esp_log.h"
#include "steering_engine.h"
#include "esp_timer.h"
#define SERVO_MIN_PULSEWIDTH 500  // Minimum pulse width in microsecond
#define SERVO_MAX_PULSEWIDTH 2500 // Maximum pulse width in microsecond
#define SERVO_MAX_DEGREE 180      // Maximum angle in degree

static const char *TAG = "STEERING_ENGINE";
static uint32_t current_angle = 0;

// 初始化舵机控制
void servo_init(gpio_num_t gpio_num) {
    ESP_LOGI(TAG, "Initializing servo on GPIO %d", gpio_num);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, gpio_num);
    mcpwm_config_t pwm_config;
    pwm_config.frequency = 50;    // Frequency = 50Hz, period = 20ms
    pwm_config.cmpr_a = 0;        // Duty cycle of PWMxA = 0
    pwm_config.cmpr_b = 0;        // Duty cycle of PWMxB = 0
    pwm_config.counter_mode = MCPWM_UP_COUNTER;
    pwm_config.duty_mode = MCPWM_DUTY_MODE_0;
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
}

// 设置舵机到指定角度
void set_servo_angle(uint32_t angle) {
    if (angle > SERVO_MAX_DEGREE) {
        // angle = SERVO_MAX_DEGREE;
        // 测试
        angle = 0;
    }
    uint32_t pulsewidth = SERVO_MIN_PULSEWIDTH + ((SERVO_MAX_PULSEWIDTH - SERVO_MIN_PULSEWIDTH) * angle) / SERVO_MAX_DEGREE;
    // ESP_LOGW(TAG, "Setting servo angle to %d degrees (pulse width: %d us)", angle, pulsewidth);
    // ESP_LOGI(TAG, "Setting servo angle to %d degrees (pulse width: %d us)", angle, pulsewidth);
    mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, pulsewidth);
    current_angle = angle;
}

// 增加角度
void increase_servo_angle(uint32_t increment) {
    ESP_LOGI(TAG, "Increasing servo angle by %d degrees", increment);
    set_servo_angle(current_angle + increment);
}

// 减少角度
void decrease_servo_angle(uint32_t decrement) {
    ESP_LOGI(TAG, "Decreasing servo angle by %d degrees", decrement);
    if (current_angle < decrement) {
        set_servo_angle(0);
    } else {
        set_servo_angle(current_angle - decrement);
    }
}

// 获取当前舵机角度
uint32_t get_current_servo_angle() {
    ESP_LOGI(TAG, "Current servo angle is %d degrees", current_angle);
    return current_angle;
}
// 业务样例1-摇尾巴

// 舵机模仿摇尾巴业务

// 摇尾巴次数
static uint32_t wag_tail_count = 0;
// 摇尾巴定时器
static esp_timer_handle_t wag_tail_timer_handle = NULL;
void wag_tail_timer_callback(void *arg) {
    static bool servo_angle_flag = false;
    servo_angle_flag = !servo_angle_flag;
    if(wag_tail_count > 0) {
        set_servo_angle(servo_angle_flag ? MAX_ANGLE : MIN_ANGLE);
        if(wag_tail_count != NEVER_STOP) {
            wag_tail_count--;
        }
        ESP_LOGI(TAG, "Wagging tail count: 0x%x (%d)", wag_tail_count, wag_tail_count);
    } else {
        wag_tail_stop();
    }
}

uint32_t get_wag_tail_period(WAG_TAIL_GEAR gear) {
    uint32_t period = 0;
    switch (gear)
    {
    case WAG_TAIL_GEAR_1:
        period = WAG_TAIL_GEAR_1_PERIOD;
        break;
    case WAG_TAIL_GEAR_2:
        period = WAG_TAIL_GEAR_2_PERIOD;
        break;
    case WAG_TAIL_GEAR_3:
        period = WAG_TAIL_GEAR_3_PERIOD;
        break;
    case WAG_TAIL_GEAR_4:
        period = WAG_TAIL_GEAR_4_PERIOD;
        break;
    case WAG_TAIL_GEAR_5:
        period = WAG_TAIL_GEAR_5_PERIOD;
        break;  
    default:
        period = WAG_TAIL_GEAR_5_PERIOD;
        break;
    }
    return period;
}

// 如果要一直执行，则count为NEVER_STOP
void wag_tail_start(uint32_t gear, uint32_t count) {
    ESP_LOGI(TAG, "Wagging tail start");
    uint32_t period = get_wag_tail_period(gear);
    wag_tail_count = count * 2;

    if (wag_tail_timer_handle == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = wag_tail_timer_callback,
            .name = "wag_tail_timer"
        };
        esp_timer_create(&timer_args, &wag_tail_timer_handle);
        esp_timer_start_periodic(wag_tail_timer_handle, period * 1000);
    } else {
        esp_timer_start_periodic(wag_tail_timer_handle, period * 1000);
    }
}

void wag_tail_stop() {
    ESP_LOGI(TAG, "Wagging tail stop");
    set_servo_angle(MID_ANGLE);
    if(wag_tail_timer_handle != NULL) {
        esp_timer_stop(wag_tail_timer_handle);
    }
}
