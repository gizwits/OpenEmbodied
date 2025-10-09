#include "motor_driver.h"
#include <esp_timer.h>
#include <esp_log.h>

static const char* TAG = "MotorDriver";

MotorDriver::MotorDriver() 
    : motor_a_state_(MOTOR_STOP)
    , motor_b_state_(MOTOR_STOP)
    , motor_a_speed_(100)
    , motor_b_speed_(100)
    , initialized_(false)
    , adc_handle_(nullptr)
    , potentiometer_channel_(ADC_CHANNEL_1)
    , action_timer_(nullptr)
    , action_running_(false)
    , potentiometer_task_handle_(nullptr)
    , potentiometer_monitoring_(false)
    , potentiometer_interval_ms_(100) {
}

MotorDriver::~MotorDriver() {
    Deinitialize();
}

bool MotorDriver::Initialize() {
    ESP_LOGI(TAG, "MotorDriver::Initialize() called");
    
    if (initialized_) {
        ESP_LOGW(TAG, "Motor driver already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Starting motor driver initialization...");

    // 配置GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    // 配置电机A控制引脚
    io_conf.pin_bit_mask = (1ULL << MOTOR_A_IN1) | (1ULL << MOTOR_A_IN2);
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure motor A GPIO pins");
        return false;
    }

    // 配置电机B控制引脚
    io_conf.pin_bit_mask = (1ULL << MOTOR_B_IN3) | (1ULL << MOTOR_B_IN4);
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure motor B GPIO pins");
        return false;
    }

    // 配置电机C控制引脚
    io_conf.pin_bit_mask = (1ULL << MOTOR_C_IN1) | (1ULL << MOTOR_C_IN2);
    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure motor C GPIO pins");
        return false;
    }


    // 初始化PWM
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.timer_num = PWM_TIMER;
    timer_conf.duty_resolution = PWM_RESOLUTION;
    timer_conf.freq_hz = PWM_FREQUENCY;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWM timer");
        return false;
    }

    // 配置PWM通道
    ledc_channel_config_t channel_conf = {};
    channel_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_conf.channel = MOTOR_A_PWM_CHANNEL;
    channel_conf.timer_sel = PWM_TIMER;
    channel_conf.intr_type = LEDC_INTR_DISABLE;
    channel_conf.gpio_num = MOTOR_A_IN1;  // 使用IN1作为PWM输出
    channel_conf.duty = 0;
    channel_conf.hpoint = 0;
    if (ledc_channel_config(&channel_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure motor A PWM channel");
        return false;
    }

    channel_conf.channel = MOTOR_B_PWM_CHANNEL;
    channel_conf.gpio_num = MOTOR_B_IN3;  // 使用IN3作为PWM输出
    if (ledc_channel_config(&channel_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure motor B PWM channel");
        return false;
    }

    // 初始化ADC
    ESP_LOGI(TAG, "Initializing ADC unit...");
    adc_oneshot_unit_init_cfg_t adc_init_cfg = {};
    adc_init_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&adc_init_cfg, &adc_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC unit");
        return false;
    }
    ESP_LOGI(TAG, "ADC unit initialized successfully");

    // 配置ADC通道
    ESP_LOGI(TAG, "Configuring ADC channel %d...", potentiometer_channel_);
    adc_oneshot_chan_cfg_t adc_chan_cfg = {};
    adc_chan_cfg.bitwidth = ADC_BITWIDTH_12;
    adc_chan_cfg.atten = ADC_ATTEN_DB_12;  // 0-3.3V范围
    if (adc_oneshot_config_channel(adc_handle_, potentiometer_channel_, &adc_chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel");
        adc_oneshot_del_unit(adc_handle_);
        return false;
    }
    ESP_LOGI(TAG, "ADC channel configured successfully");

    // 创建动作定时器
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = ActionTimerCallback;
    timer_args.arg = this;
    timer_args.name = "motor_action_timer";
    if (esp_timer_create(&timer_args, &action_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create action timer");
        adc_oneshot_del_unit(adc_handle_);
        return false;
    }

    // 初始化电机状态为停止
    StopAll();

    initialized_ = true;
    ESP_LOGI(TAG, "Motor driver initialized successfully");

    // 启动电位器监控（默认200ms间隔）
    // if (!StartPotentiometerMonitoring(100)) {
    //     ESP_LOGW(TAG, "Failed to start potentiometer monitoring during initialization");
    // }
    return true;
}

void MotorDriver::Deinitialize() {
    if (!initialized_) {
        return;
    }

    // 停止所有动作
    StopAction();
    StopAll();

    // 停止电位器监控任务
    StopPotentiometerMonitoring();

    // 删除定时器
    if (action_timer_) {
        esp_timer_delete(action_timer_);
        action_timer_ = nullptr;
    }

    // 删除ADC单元
    if (adc_handle_) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }

    initialized_ = false;
    ESP_LOGI(TAG, "Motor driver deinitialized");
}

void MotorDriver::SetMotorState(MotorId motor_id, MotorState state, int speed) {
    if (!initialized_) {
        ESP_LOGW(TAG, "Motor driver not initialized");
        return;
    }

    // 限制速度范围
    speed = (speed < 0) ? 0 : (speed > 100) ? 100 : speed;

    if (motor_id == MOTOR_A) {
        motor_a_state_ = state;
        motor_a_speed_ = speed;
        
        switch (state) {
            case MOTOR_STOP:
                SetGpioLevel(MOTOR_A_IN1, 0);
                SetGpioLevel(MOTOR_A_IN2, 0);
                SetPwmDuty(MOTOR_A_PWM_CHANNEL, 0);
                break;
                
            case MOTOR_FORWARD:
                SetGpioLevel(MOTOR_A_IN1, 1);
                SetGpioLevel(MOTOR_A_IN2, 0);
                SetPwmDuty(MOTOR_A_PWM_CHANNEL, SpeedToDuty(speed));
                break;
                
            case MOTOR_REVERSE:
                SetGpioLevel(MOTOR_A_IN1, 0);
                SetGpioLevel(MOTOR_A_IN2, 1);
                SetPwmDuty(MOTOR_A_PWM_CHANNEL, SpeedToDuty(speed));
                break;
                
            case MOTOR_BRAKE:
                SetGpioLevel(MOTOR_A_IN1, 1);
                SetGpioLevel(MOTOR_A_IN2, 1);
                SetPwmDuty(MOTOR_A_PWM_CHANNEL, 0);
                break;
        }
        
        ESP_LOGI(TAG, "Motor A: state=%d, speed=%d", state, speed);
        
    } else if (motor_id == MOTOR_B) {
        motor_b_state_ = state;
        motor_b_speed_ = speed;
        
        switch (state) {
            case MOTOR_STOP:
                SetGpioLevel(MOTOR_B_IN3, 0);
                SetGpioLevel(MOTOR_B_IN4, 0);
                SetPwmDuty(MOTOR_B_PWM_CHANNEL, 0);
                break;
                
            case MOTOR_FORWARD:
                SetGpioLevel(MOTOR_B_IN3, 1);
                SetGpioLevel(MOTOR_B_IN4, 0);
                SetPwmDuty(MOTOR_B_PWM_CHANNEL, SpeedToDuty(speed));
                break;
                
            case MOTOR_REVERSE:
                SetGpioLevel(MOTOR_B_IN3, 0);
                SetGpioLevel(MOTOR_B_IN4, 1);
                SetPwmDuty(MOTOR_B_PWM_CHANNEL, SpeedToDuty(speed));
                break;
                
            case MOTOR_BRAKE:
                SetGpioLevel(MOTOR_B_IN3, 1);
                SetGpioLevel(MOTOR_B_IN4, 1);
                SetPwmDuty(MOTOR_B_PWM_CHANNEL, 0);
                break;
        }
        
        ESP_LOGI(TAG, "Motor B: state=%d, speed=%d", state, speed);
    }
}

void MotorDriver::SetMotorSpeed(MotorId motor_id, int speed) {
    if (motor_id == MOTOR_A) {
        SetMotorState(MOTOR_A, motor_a_state_, speed);
    } else if (motor_id == MOTOR_B) {
        SetMotorState(MOTOR_B, motor_b_state_, speed);
    }
}

void MotorDriver::StopAll() {
    SetMotorState(MOTOR_A, MOTOR_STOP, 0);
    SetMotorState(MOTOR_B, MOTOR_STOP, 0);
    SetMotorCStop();
    rear_leg_running_ = false;
}

void MotorDriver::BrakeAll() {
    SetMotorState(MOTOR_A, MOTOR_BRAKE, 0);
    SetMotorState(MOTOR_B, MOTOR_BRAKE, 0);
}

void MotorDriver::StopRearLeg() {
    rear_leg_running_ = false;
    SetMotorCStop();
}

void MotorDriver::SetMotorCStop() {
    SetGpioLevel(MOTOR_C_IN1, 0);
    SetGpioLevel(MOTOR_C_IN2, 0);
}

void MotorDriver::SetMotorCForward() {
    SetGpioLevel(MOTOR_C_IN1, 1);
    SetGpioLevel(MOTOR_C_IN2, 0);
}

void MotorDriver::SetMotorCReverse() {
    SetGpioLevel(MOTOR_C_IN1, 0);
    SetGpioLevel(MOTOR_C_IN2, 1);
}

bool MotorDriver::MoveRearLegToFrontmost(int timeout_ms) {
    const int target = 900;
    const int threshold = 20; // 允许的误差
    int elapsed = 0;

    // 朝前端运动
    rear_leg_running_ = true;
    SetMotorCForward();

    while (elapsed < timeout_ms && rear_leg_running_) {
        int adc_raw = ReadPotentiometerRaw();
        // 已经在目标以下，视为已到达或越界到前端
        if (adc_raw <= target - threshold) {
            SetMotorCStop();
            ESP_LOGI(TAG, "Rear leg already at/below frontmost: adc=%d", adc_raw);
            rear_leg_running_ = false;
            return true;
        }
        if (adc_raw >= target - threshold && adc_raw <= target + threshold) {
            SetMotorCStop();
            ESP_LOGI(TAG, "Rear leg reached frontmost: adc=%d", adc_raw);
            rear_leg_running_ = false;
            return true;
        }
        if ((elapsed % 100) == 0) {
            ESP_LOGI(TAG, "后腿前进中：adc=%d (目标=%d±%d)", adc_raw, target, threshold);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    SetMotorCStop();
    int adc_last = ReadPotentiometerRaw();
    if (!rear_leg_running_) {
        ESP_LOGW(TAG, "后腿移动到最前端被打断，最后adc=%d", adc_last);
    } else {
        ESP_LOGW(TAG, "后腿移动到最前端超时，最后adc=%d，目标=%d±%d", adc_last, target, threshold);
    }
    rear_leg_running_ = false;
    return false;
}

bool MotorDriver::MoveRearLegToBackmost(int timeout_ms) {
    const int target = 2750;
    const int threshold = 20; // 允许的误差
    int elapsed = 0;

    // 朝后端运动
    rear_leg_running_ = true;
    SetMotorCReverse();
    {
        int adc_start = ReadPotentiometerRaw();
        ESP_LOGI(TAG, "后腿：开始向最后端移动，目标=%d，当前=%d", target, adc_start);
    }

    while (elapsed < timeout_ms && rear_leg_running_) {
        int adc_raw = ReadPotentiometerRaw();
        // 已经在目标以上，视为已到达或越界到后端
        if (adc_raw >= target + threshold) {
            SetMotorCStop();
            ESP_LOGI(TAG, "Rear leg already at/above backmost: adc=%d", adc_raw);
            rear_leg_running_ = false;
            return true;
        }
        if (adc_raw >= target - threshold && adc_raw <= target + threshold) {
            SetMotorCStop();
            ESP_LOGI(TAG, "Rear leg reached backmost: adc=%d", adc_raw);
            rear_leg_running_ = false;
            return true;
        }
        if ((elapsed % 100) == 0) {
            ESP_LOGI(TAG, "后腿后退中：adc=%d (目标=%d±%d)", adc_raw, target, threshold);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    SetMotorCStop();
    int adc_last = ReadPotentiometerRaw();
    if (!rear_leg_running_) {
        ESP_LOGW(TAG, "后腿移动到最后端被打断，最后adc=%d", adc_last);
    } else {
        ESP_LOGW(TAG, "后腿移动到最后端超时，最后adc=%d，目标=%d±%d", adc_last, target, threshold);
    }
    rear_leg_running_ = false;
    return false;
}

bool MotorDriver::NormalizeRearLegPosition(int timeout_ms) {
    const int front_target = 900;
    const int back_target = 2750;
    const int threshold = 20;
    int adc = ReadPotentiometerRaw();
    ESP_LOGI(TAG, "后腿规范位置：当前adc=%d (前=%d±%d, 后=%d±%d)", adc, front_target, threshold, back_target, threshold);

    if (adc <= front_target - threshold) {
        return MoveRearLegToFrontmost(timeout_ms);
    }
    if (adc >= back_target + threshold) {
        return MoveRearLegToBackmost(timeout_ms);
    }
    ESP_LOGI(TAG, "后腿已在允许范围内，无需动作");
    return true;
}

bool MotorDriver::MoveRearLegToMiddle(int timeout_ms) {
    const int front_target = 900;
    const int back_target = 2750;
    const int target = (front_target + back_target) / 2; // 中点约 1825
    const int threshold = 25; // 中点略放宽
    int elapsed = 0;

    rear_leg_running_ = true;

    while (elapsed < timeout_ms && rear_leg_running_) {
        int adc_raw = ReadPotentiometerRaw();
        // 到位判定
        if (adc_raw >= target - threshold && adc_raw <= target + threshold) {
            SetMotorCStop();
            ESP_LOGI(TAG, "后腿到中间：adc=%d(目标=%d±%d)", adc_raw, target, threshold);
            rear_leg_running_ = false;
            return true;
        }

        // 根据位置选择方向：小于目标则朝后端，大于目标则朝前端
        if (adc_raw < target) {
            SetMotorCReverse();
        } else {
            SetMotorCForward();
        }

        if ((elapsed % 100) == 0) {
            ESP_LOGI(TAG, "后腿居中中：adc=%d -> 目标=%d±%d", adc_raw, target, threshold);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
    }

    SetMotorCStop();
    int adc_last = ReadPotentiometerRaw();
    if (!rear_leg_running_) {
        ESP_LOGW(TAG, "后腿居中被打断，最后adc=%d", adc_last);
    } else {
        ESP_LOGW(TAG, "后腿居中超时，最后adc=%d(目标=%d±%d)", adc_last, target, threshold);
    }
    rear_leg_running_ = false;
    return false;
}

void MotorDriver::ExecuteAction(ActionType action_type, int duration_ms, int speed, 
                               std::function<void()> callback) {
    if (!initialized_) {
        ESP_LOGW(TAG, "Motor driver not initialized");
        return;
    }

    // 停止当前动作
    StopAction();

    // 设置新动作
    current_action_.type = action_type;
    current_action_.duration_ms = duration_ms;
    current_action_.speed_a = speed;
    current_action_.speed_b = speed;
    current_action_.callback = callback;

    // 根据动作类型设置电机状态
    switch (action_type) {
        case ACTION_FORWARD:
            SetMotorState(MOTOR_A, MOTOR_FORWARD, speed);
            SetMotorState(MOTOR_B, MOTOR_FORWARD, speed);
            break;
            
        case ACTION_BACKWARD:
            SetMotorState(MOTOR_A, MOTOR_REVERSE, speed);
            SetMotorState(MOTOR_B, MOTOR_REVERSE, speed);
            break;
            
        case ACTION_LEFT:
            SetMotorState(MOTOR_A, MOTOR_REVERSE, speed);
            SetMotorState(MOTOR_B, MOTOR_FORWARD, speed);
            break;
            
        case ACTION_RIGHT:
            SetMotorState(MOTOR_A, MOTOR_FORWARD, speed);
            SetMotorState(MOTOR_B, MOTOR_REVERSE, speed);
            break;
            
        case ACTION_STOP:
            StopAll();
            break;
            
        case ACTION_BRAKE:
            BrakeAll();
            break;
            
        case ACTION_CUSTOM:
            // 自定义动作需要调用ExecuteCustomAction
            ESP_LOGW(TAG, "Use ExecuteCustomAction for custom actions");
            return;
    }

    // 如果设置了持续时间，启动定时器
    if (duration_ms > 0) {
        action_running_ = true;
        esp_timer_start_once(action_timer_, duration_ms * 1000);
        ESP_LOGI(TAG, "Action started: type=%d, duration=%dms, speed=%d", 
                 action_type, duration_ms, speed);
    }
}

void MotorDriver::ExecuteCustomAction(const Action& action) {
    if (!initialized_) {
        ESP_LOGW(TAG, "Motor driver not initialized");
        return;
    }

    // 停止当前动作
    StopAction();

    // 设置新动作
    current_action_ = action;

    // 设置电机状态
    SetMotorState(MOTOR_A, action.speed_a > 0 ? MOTOR_FORWARD : MOTOR_STOP, action.speed_a);
    SetMotorState(MOTOR_B, action.speed_b > 0 ? MOTOR_FORWARD : MOTOR_STOP, action.speed_b);

    // 如果设置了持续时间，启动定时器
    if (action.duration_ms > 0) {
        action_running_ = true;
        esp_timer_start_once(action_timer_, action.duration_ms * 1000);
        ESP_LOGI(TAG, "Custom action started: duration=%dms, speed_a=%d, speed_b=%d", 
                 action.duration_ms, action.speed_a, action.speed_b);
    }
}

void MotorDriver::StopAction() {
    if (action_timer_ && action_running_) {
        esp_timer_stop(action_timer_);
        action_running_ = false;
        ESP_LOGI(TAG, "Action stopped");
    }
}

MotorDriver::MotorState MotorDriver::GetMotorState(MotorId motor_id) const {
    if (motor_id == MOTOR_A) {
        return motor_a_state_;
    } else if (motor_id == MOTOR_B) {
        return motor_b_state_;
    }
    return MOTOR_STOP;
}

int MotorDriver::GetMotorSpeed(MotorId motor_id) const {
    if (motor_id == MOTOR_A) {
        return motor_a_speed_;
    } else if (motor_id == MOTOR_B) {
        return motor_b_speed_;
    }
    return 0;
}

void MotorDriver::MoveForward(int duration_ms, int speed) {
    ExecuteAction(ACTION_FORWARD, duration_ms, speed);
}

void MotorDriver::MoveBackward(int duration_ms, int speed) {
    ExecuteAction(ACTION_BACKWARD, duration_ms, speed);
}

void MotorDriver::TurnLeft(int duration_ms, int speed) {
    ExecuteAction(ACTION_LEFT, duration_ms, speed);
}

void MotorDriver::TurnRight(int duration_ms, int speed) {
    ExecuteAction(ACTION_RIGHT, duration_ms, speed);
}

void MotorDriver::Stop() {
    ExecuteAction(ACTION_STOP);
}

void MotorDriver::Brake() {
    ExecuteAction(ACTION_BRAKE);
}

void MotorDriver::SetGpioLevel(gpio_num_t gpio_num, int level) {
    gpio_set_level(gpio_num, level);
}

void MotorDriver::SetPwmDuty(ledc_channel_t channel, uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

uint32_t MotorDriver::SpeedToDuty(int speed_percent) const {
    if (speed_percent <= 0) return 0;
    if (speed_percent >= 100) return 255;
    return (speed_percent * 255) / 100;
}

void MotorDriver::OnActionTimer() {
    if (action_running_) {
        action_running_ = false;
        
        // 停止电机
        StopAll();
        
        // 执行回调
        if (current_action_.callback) {
            current_action_.callback();
        }
        
        ESP_LOGI(TAG, "Action completed");
    }
}

void MotorDriver::ActionTimerCallback(void* arg) {
    MotorDriver* driver = static_cast<MotorDriver*>(arg);
    driver->OnActionTimer();
}

// 电位器ADC读取方法实现
int MotorDriver::ReadPotentiometerRaw() {
    if (!initialized_ || !adc_handle_) {
        ESP_LOGW(TAG, "Motor driver not initialized or ADC not available (init=%d, handle=%p)", 
                 initialized_, adc_handle_);
        return 0;
    }

    int adc_raw = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle_, potentiometer_channel_, &adc_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ADC: %s", esp_err_to_name(ret));
        return 0;
    }

    return adc_raw;
}

float MotorDriver::ReadPotentiometerVoltage() {
    int adc_raw = ReadPotentiometerRaw();
    if (adc_raw == 0) {
        return 0.0f;
    }

    // 12-bit ADC: 0-4095 -> 0.0-3.3V
    float voltage = (float)adc_raw * 3.3f / 4095.0f;
    return voltage;
}

// 电位器监控任务实现
bool MotorDriver::StartPotentiometerMonitoring(int interval_ms) {
    ESP_LOGI(TAG, "StartPotentiometerMonitoring called with interval: %dms", interval_ms);
    
    if (!initialized_) {
        ESP_LOGE(TAG, "Motor driver not initialized");
        return false;
    }

    if (potentiometer_monitoring_) {
        ESP_LOGW(TAG, "Potentiometer monitoring already running");
        return true;
    }

    if (!adc_handle_) {
        ESP_LOGE(TAG, "ADC handle is null");
        return false;
    }

    potentiometer_interval_ms_ = interval_ms;
    potentiometer_monitoring_ = true;

    ESP_LOGI(TAG, "Creating potentiometer monitoring task...");
    BaseType_t ret = xTaskCreate(
        PotentiometerMonitoringTask,
        "potentiometer_monitor",
        4096,  // 栈大小
        this,  // 参数
        5,     // 优先级
        &potentiometer_task_handle_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create potentiometer monitoring task, ret=%d", ret);
        potentiometer_monitoring_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Potentiometer monitoring task created successfully");
    ESP_LOGI(TAG, "Potentiometer monitoring started (interval: %dms)", interval_ms);
    return true;
}

void MotorDriver::StopPotentiometerMonitoring() {
    if (!potentiometer_monitoring_) {
        return;
    }

    potentiometer_monitoring_ = false;

    if (potentiometer_task_handle_) {
        vTaskDelete(potentiometer_task_handle_);
        potentiometer_task_handle_ = nullptr;
    }

    ESP_LOGI(TAG, "Potentiometer monitoring stopped");
}

void MotorDriver::PotentiometerMonitoringTask(void* arg) {
    MotorDriver* driver = static_cast<MotorDriver*>(arg);
    
    ESP_LOGI(TAG, "Potentiometer monitoring task started");
    ESP_LOGI(TAG, "Task running on core %d", xPortGetCoreID());
    
    int loop_count = 0;
    while (driver->potentiometer_monitoring_) {
        loop_count++;
        // ESP_LOGI(TAG, "Loop %d: monitoring=%d, interval=%d", 
        //          loop_count, driver->potentiometer_monitoring_, driver->potentiometer_interval_ms_);
        
        int adc_raw = driver->ReadPotentiometerRaw();
        float voltage = driver->ReadPotentiometerVoltage();
        
        // ESP_LOGI(TAG, "Potentiometer - ADC: %d, Voltage: %.3fV", adc_raw, voltage);
        
        vTaskDelay(pdMS_TO_TICKS(driver->potentiometer_interval_ms_));
    }
    
    ESP_LOGI(TAG, "Potentiometer monitoring task ended");
    vTaskDelete(nullptr);
}
