#pragma once

#include <stdint.h>


class Servo {
public:
    // 构造函数，传入 GPIO 编号和原点角度（可选，默认0）
    Servo(int gpio_num, int origin_angle = 0);
    // 初始化 PWM
    void begin();
    // 开始运动，参数：最小角度、最大角度、速度(度/秒)、持续时间(ms)
    // 最大值建议：1000
    // 最小值建议：1
    void move(int min_angle, int max_angle, int speed_dps, int duration_ms);
    // 停止运动，并回到原点
    void stop();
    // 析构
    ~Servo();
private:
    int gpio_num_;
    int min_angle_;
    int max_angle_;
    int speed_dps_;
    int duration_ms_;
    int origin_angle_;
    bool running_;
    void* task_handle_; // FreeRTOS 任务句柄
    void set_angle(int angle);
    friend void servo_task(void* arg);
};
