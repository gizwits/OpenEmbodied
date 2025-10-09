#ifndef _MOTOR_DRIVER_H_
#define _MOTOR_DRIVER_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_adc/adc_oneshot.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string>
#include <functional>

/**
 * 电机驱动板控制类
 * 支持双电机独立控制，包括前进、后退、停止、刹车和PWM调速
 * 
 * GPIO映射关系：
 * - GPIO_NUM_4 -> IN1 (电机A控制1)
 * - GPIO_NUM_2 -> IN3 (电机B控制1)
 * - GPIO_NUM_1 -> IN4 (电机B控制2)
 * - GPIO_NUM_7 -> IN2 (电机A控制2)
 */
class MotorDriver {
public:
    // 电机编号枚举
    enum MotorId {
        MOTOR_A = 0,
        MOTOR_B = 1
    };

    // 电机状态枚举
    enum MotorState {
        MOTOR_STOP = 0,     // 停止
        MOTOR_FORWARD = 1,  // 正转
        MOTOR_REVERSE = 2,  // 反转
        MOTOR_BRAKE = 3     // 刹车
    };

    // 动作类型枚举
    enum ActionType {
        ACTION_FORWARD,     // 前进
        ACTION_BACKWARD,    // 后退
        ACTION_LEFT,        // 左转
        ACTION_RIGHT,       // 右转
        ACTION_STOP,        // 停止
        ACTION_BRAKE,       // 刹车
        ACTION_CUSTOM       // 自定义动作
    };

    // 动作结构体
    struct Action {
        ActionType type;
        int duration_ms;    // 持续时间(毫秒)，-1表示持续执行
        int speed_a;        // 电机A速度 (0-100)
        int speed_b;        // 电机B速度 (0-100)
        std::function<void()> callback; // 动作完成回调
    };

private:
    // GPIO引脚定义
    static constexpr gpio_num_t MOTOR_A_IN1 = GPIO_NUM_4;  // 电机A控制1
    static constexpr gpio_num_t MOTOR_A_IN2 = GPIO_NUM_7;  // 电机A控制2
    static constexpr gpio_num_t MOTOR_B_IN3 = GPIO_NUM_2;  // 电机B控制1
    static constexpr gpio_num_t MOTOR_B_IN4 = GPIO_NUM_6;  // 电机B控制2

    static constexpr gpio_num_t MOTOR_C_IN1 = GPIO_NUM_5;  // 电机C控制1
    static constexpr gpio_num_t MOTOR_C_IN2 = GPIO_NUM_19;  // 电机C控制1


    // PWM配置
    static constexpr ledc_channel_t MOTOR_A_PWM_CHANNEL = LEDC_CHANNEL_0;
    static constexpr ledc_channel_t MOTOR_B_PWM_CHANNEL = LEDC_CHANNEL_1;
    static constexpr ledc_timer_t PWM_TIMER = LEDC_TIMER_0;
    static constexpr uint32_t PWM_FREQUENCY = 1000;  // 1kHz
    static constexpr ledc_timer_bit_t PWM_RESOLUTION = LEDC_TIMER_8_BIT;  // 8位分辨率，0-255

    // 当前状态
    MotorState motor_a_state_;
    MotorState motor_b_state_;
    int motor_a_speed_;
    int motor_b_speed_;
    bool initialized_;

    // 电位器ADC读取
    adc_oneshot_unit_handle_t adc_handle_;
    adc_channel_t potentiometer_channel_;

    // 动作执行相关
    esp_timer_handle_t action_timer_;
    Action current_action_;
    bool action_running_;

    // 电位器监控任务
    TaskHandle_t potentiometer_task_handle_;
    bool potentiometer_monitoring_;
    int potentiometer_interval_ms_;  // 读取间隔(毫秒)

    // 后腿动作运行标志（可被打断）
    volatile bool rear_leg_running_ = false;

    // 静态定时器回调函数
    static void ActionTimerCallback(void* arg);
    
    // 静态任务函数
    static void PotentiometerMonitoringTask(void* arg);

public:
    MotorDriver();
    ~MotorDriver();

    /**
     * 初始化电机驱动板
     * @return true 初始化成功，false 初始化失败
     */
    bool Initialize();

    /**
     * 反初始化，释放资源
     */
    void Deinitialize();

    /**
     * 设置电机状态
     * @param motor_id 电机编号 (MOTOR_A 或 MOTOR_B)
     * @param state 电机状态
     * @param speed 速度 (0-100)，仅在正转/反转时有效
     */
    void SetMotorState(MotorId motor_id, MotorState state, int speed = 100);

    /**
     * 设置电机速度
     * @param motor_id 电机编号
     * @param speed 速度 (0-100)
     */
    void SetMotorSpeed(MotorId motor_id, int speed);

    /**
     * 停止所有电机
     */
    void StopAll();

    /**
     * 刹车所有电机
     */
    void BrakeAll();

    /**
     * 执行预定义动作
     * @param action_type 动作类型
     * @param duration_ms 持续时间(毫秒)，-1表示持续执行
     * @param speed 速度 (0-100)
     * @param callback 动作完成回调
     */
    void ExecuteAction(ActionType action_type, int duration_ms = -1, int speed = 100, 
                      std::function<void()> callback = nullptr);

    /**
     * 执行自定义动作
     * @param action 动作结构体
     */
    void ExecuteCustomAction(const Action& action);

    /**
     * 停止当前动作
     */
    void StopAction();

    /**
     * 检查是否有动作正在执行
     * @return true 有动作执行中，false 无动作执行
     */
    bool IsActionRunning() const { return action_running_; }

    /**
     * 获取电机当前状态
     * @param motor_id 电机编号
     * @return 电机状态
     */
    MotorState GetMotorState(MotorId motor_id) const;

    /**
     * 获取电机当前速度
     * @param motor_id 电机编号
     * @return 电机速度 (0-100)
     */
    int GetMotorSpeed(MotorId motor_id) const;

    // 电位器ADC读取方法
    /**
     * 读取电位器原始ADC值
     * @return ADC原始值 (0-4095 for 12-bit)
     */
    int ReadPotentiometerRaw();

    /**
     * 读取电位器电压值
     * @return 电压值 (0.0-3.3V)
     */
    float ReadPotentiometerVoltage();

    // 电位器监控任务相关方法
    /**
     * 启动电位器监控任务
     * @param interval_ms 读取间隔(毫秒)，默认100ms
     * @return true 启动成功，false 启动失败
     */
    bool StartPotentiometerMonitoring(int interval_ms = 100);

    /**
     * 停止电位器监控任务
     */
    void StopPotentiometerMonitoring();

    /**
     * 检查电位器监控是否运行中
     * @return true 运行中，false 未运行
     */
    bool IsPotentiometerMonitoring() const { return potentiometer_monitoring_; }

    // 预定义动作方法
    void MoveForward(int duration_ms = -1, int speed = 100);
    void MoveBackward(int duration_ms = -1, int speed = 100);
    void TurnLeft(int duration_ms = -1, int speed = 100);
    void TurnRight(int duration_ms = -1, int speed = 100);
    void Stop();
    void Brake();

    // 后腿控制（电机C）
    /**
     * 将后腿移动到最前端（电位器原始值≈900）
     * @param timeout_ms 超时时间，毫秒，默认4000
     * @return true 达到目标，false 超时未到达
     */
    bool MoveRearLegToFrontmost(int timeout_ms = 4000);

    /**
     * 将后腿移动到最后端（电位器原始值≈2790）
     * @param timeout_ms 超时时间，毫秒，默认4000
     * @return true 达到目标，false 超时未到达
     */
    bool MoveRearLegToBackmost(int timeout_ms = 4000);

    /**
     * 将后腿移动到中间站立位置（约在前后目标中点）
     * @param timeout_ms 超时时间，毫秒，默认4000
     * @return true 达到目标，false 超时或被打断
     */
    bool MoveRearLegToMiddle(int timeout_ms = 4000);

    /**
     * 立即停止后腿运动（中断进行中的到位动作）
     */
    void StopRearLeg();

    /**
     * 规范后腿位置：
     *  - adc < 900-阈值 时，移动到最前端
     *  - adc > 2750+阈值 时，移动到最后端
     *  - 否则不动作
     */
    bool NormalizeRearLegPosition(int timeout_ms = 5000);

private:
    /**
     * 设置GPIO输出
     * @param gpio_num GPIO编号
     * @param level 输出电平
     */
    void SetGpioLevel(gpio_num_t gpio_num, int level);

    /**
     * 设置PWM输出
     * @param channel PWM通道
     * @param duty PWM占空比 (0-255)
     */
    void SetPwmDuty(ledc_channel_t channel, uint32_t duty);

    /**
     * 将速度百分比转换为PWM占空比
     * @param speed_percent 速度百分比 (0-100)
     * @return PWM占空比 (0-255)
     */
    uint32_t SpeedToDuty(int speed_percent) const;

    /**
     * 执行动作定时器回调
     */
    void OnActionTimer();

    // 后腿电机底层控制
    void SetMotorCStop();
    void SetMotorCForward();  // 朝向前端
    void SetMotorCReverse();  // 朝向后端
};

#endif // _MOTOR_DRIVER_H_
