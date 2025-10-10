#ifndef __MOTOR_CONTROL_H__
#define __MOTOR_CONTROL_H__

#include <driver/gpio.h>
#include <driver/ledc.h>
#include "config.h"

class MotorControl {
private:
    bool initialized_ = false;
    bool running_ = false;
    uint8_t speed_ = 0;
    bool forward_ = true;
    
    // PWM配置
    static constexpr ledc_channel_t MOTOR_LEDC_CHANNEL = LEDC_CHANNEL_0;
    static constexpr ledc_timer_t MOTOR_LEDC_TIMER = LEDC_TIMER_0;
    static constexpr uint32_t MOTOR_PWM_FREQ = 20000;  // 20kHz PWM频率，更适合电机控制

public:
    MotorControl();
    ~MotorControl();

    void Initialize();
    void Start();
    void Stop();
    void SetSpeed(uint8_t speed);
    void SetDirection(bool forward);
    
    bool IsRunning() const { return running_; }
    uint8_t GetSpeed() const { return speed_; }
    bool GetDirection() const { return forward_; }
    bool IsInitialized() const { return initialized_; }
};

#endif // __MOTOR_CONTROL_H__
