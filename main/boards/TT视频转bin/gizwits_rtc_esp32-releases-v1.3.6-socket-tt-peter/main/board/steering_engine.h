#ifndef STEERING_ENGINE_H
#define STEERING_ENGINE_H

#include "driver/gpio.h"
#include "driver/mcpwm.h"

// 定义舵机的最小和最大脉冲宽度以及最大角度
#define SERVO_MIN_PULSEWIDTH 500  // Minimum pulse width in microsecond
#define SERVO_MAX_PULSEWIDTH 2500 // Maximum pulse width in microsecond
#define SERVO_MAX_DEGREE 180      // Maximum angle in degree

// 函数声明
void servo_init(gpio_num_t gpio_num);
void set_servo_angle(uint32_t angle);
void increase_servo_angle(uint32_t increment);
void decrease_servo_angle(uint32_t decrement);


// 摇尾巴挡位
typedef enum WAG_TAIL_GEAR {
    WAG_TAIL_GEAR_1 = 1,
    WAG_TAIL_GEAR_2 = 2,
    WAG_TAIL_GEAR_3 = 3,
    WAG_TAIL_GEAR_4 = 4,
    WAG_TAIL_GEAR_5 = 5
} WAG_TAIL_GEAR;
#define WAG_TAIL_GEAR_1_PERIOD 1000 // 慢
#define WAG_TAIL_GEAR_2_PERIOD 800
#define WAG_TAIL_GEAR_3_PERIOD 600
#define WAG_TAIL_GEAR_4_PERIOD 400
#define WAG_TAIL_GEAR_5_PERIOD 200

// 摇尾巴幅度
#define MIN_ANGLE 60
#define MID_ANGLE 90
#define MAX_ANGLE 120
#define NEVER_STOP 0xFFFFFFFF

// 摇尾巴开始
void wag_tail_start(uint32_t gear, uint32_t count);

// 停止摇尾巴
void wag_tail_stop();


#endif // STEERING_ENGINE_H 