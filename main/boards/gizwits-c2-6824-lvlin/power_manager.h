#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include "config.h"
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "vb6824.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include <inttypes.h>
#include "settings.h"

// Battery ADC configuration
#define BAT_ADC_CHANNEL  ADC_CHANNEL_3  // Battery voltage ADC channel
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11 // ADC attenuation
#define BAT_ADC_UNIT     ADC_UNIT_1
#define POWER_CHARGE_LED_PIN GPIO_NUM_NC

class PowerManager {
private:
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    gpio_num_t bat_led_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    int adc_value;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    uint32_t average_adc = 0;
    bool is_charging_ = false;
    bool was_charging_ = false;  // 上一次的充电状态

    static constexpr uint8_t MAX_CHANGE_COUNT = 8;
    static constexpr uint32_t TIME_LIMIT = 2000000; // 2 seconds in microseconds

    uint8_t change_count_ = 0;  // 记录状态变化次数
    uint64_t last_change_time_ = 0;  // 最后一次状态变化的时间戳（微秒）

    adc_oneshot_unit_handle_t adc_handle_;

    // 充电模拟相关变量
    static constexpr uint32_t BATTERY_CAPACITY_MAH = 1000;  // 电池容量 1000mAh
    static constexpr uint32_t CHARGE_CURRENT_MA = 260;      // 充电电流 500mA
    static constexpr uint32_t BATTERY_RECORD_INTERVAL_MS = 60000;  // 非充电状态下每60秒记录一次电量
    static constexpr uint32_t CHARGE_SIMULATION_SAVE_INTERVAL_MS = 10000;  // 充电模拟状态下每10秒保存一次电量
    
    bool is_charging_simulation_active_ = false;  // 是否正在进行充电模拟
    bool is_fully_charged_ = false;               // 是否已充满
    uint8_t charge_start_level_ = 0;              // 开始充电时的电量百分比
    int64_t charge_start_time_us_ = 0;            // 开始充电的时间（微秒）
    int64_t last_battery_record_time_us_ = 0;    // 上次记录电量的时间（微秒）
    int64_t last_charge_save_time_us_ = 0;       // 上次保存充电模拟电量的时间（微秒）
    uint8_t last_recorded_level_ = 0;             // 上次记录的电量

    // 电压-电量对照表
    static constexpr struct VoltageSocPair {
        uint16_t adcValue; // ADC value
        uint8_t soc;       // State of Charge (percentage of battery capacity)
    } dischargeCurve[] = {
        {4140, 100}, // 100%
        {4104, 95},  // 下降36mV
        {4068, 90},  // 下降36mV
        {4032, 85},  // 下降36mV
        {3996, 80},  // 下降36mV
        {3960, 75},  // 下降36mV
        {3924, 70},  // 下降36mV
        {3888, 65},  // 下降36mV
        {3852, 60},  // 下降36mV
        {3829, 55},  // 下降23mV（过渡段开始）
        {3808, 50},  // 下降21mV
        {3787, 45},  // 下降21mV
        {3766, 40},  // 下降21mV
        {3745, 35},  // 下降21mV
        {3724, 30},  // 下降21mV
        {3703, 25},  // 下降21mV
        {3672, 20},  // 下降31mV
        {3570, 15},  // 下降102mV
        {3520, 10},
        {3450, 5},
        {3400, 0},
        // {3420, 10},  // 下降150mV（低电量段开始）
        // {3220, 5},   // 下降200mV
        // {3000, 0}    // 下降220mV
    };

    // 查表函数
    uint8_t estimate_soc(uint16_t voltage) {
        uint16_t closest_voltage = dischargeCurve[0].adcValue;
        uint8_t closest_soc = dischargeCurve[0].soc;
        uint16_t min_diff = abs(voltage - closest_voltage);

        for (size_t i = 1; i < sizeof(dischargeCurve) / sizeof(dischargeCurve[0]); i++) {
            uint16_t diff = abs(voltage - dischargeCurve[i].adcValue);
            if (diff < min_diff) {
                min_diff = diff;
                closest_voltage = dischargeCurve[i].adcValue;
                closest_soc = dischargeCurve[i].soc;
            }
        }
        return closest_soc;
    }

    void CheckBatteryStatus() {

        #define BATTERY_FULL_VOLTAGE 4300
        #define BATTERY_NOT_CHARGING_VOLTAGE 4200

        static uint8_t not_charging_count = 0;
        static uint32_t log_counter = 0;
        static uint32_t last_voltage = 0;
        
        // 在读取ADC之前，先保存当前电量（如果是非充电状态）
        // 这样可以确保在检测到电压突然升高时，使用的是真实的非充电状态电量
        uint8_t battery_level_before_adc = battery_level_;
        
        // 先读取ADC
        ReadBatteryAdcData();
        uint32_t voltage = average_adc == 0 ? adc_value*2 : average_adc*2;
        
        bool previous_charging_state = is_charging_;
        
        if (voltage > BATTERY_FULL_VOLTAGE) {
            is_charging_ = true;
            not_charging_count = 0; // 重置计数器
        } else if (voltage < BATTERY_NOT_CHARGING_VOLTAGE) {
            if (is_charging_) {
                not_charging_count++;
                if (not_charging_count >= 20) {
                    is_charging_ = false;
                    not_charging_count = 0;
                }
            }
        }
        
        last_voltage = voltage;
        
        // 每50次（约5秒）打印一次状态信息，或者状态变化时立即打印
        if (previous_charging_state != is_charging_ || (log_counter++ % 50 == 0)) {
            ESP_LOGI("PowerManager", "[状态检测] 电压: %" PRIu32 "mV, 电量: %d%%, 充电状态: %s, 模拟激活: %s", 
                     voltage, battery_level_, 
                     is_charging_ ? "是" : "否",
                     is_charging_simulation_active_ ? "是" : "否");
        }

        // 处理充电状态变化和充电模拟
        HandleChargingStateChange();
        
        // 非充电状态下定期记录电量
        if (!is_charging_) {
            RecordBatteryLevelWhenNotCharging();
        }
    }

    
    void ReadBatteryAdcData() {
        static uint8_t times = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, adc_channel_, &adc_value));

        adc_values_[adc_values_index_] = adc_value;
        adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
        if (adc_values_count_ < ADC_VALUES_COUNT) {
            adc_values_count_++;
        }
        average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];

        }
        average_adc /= adc_values_count_;

        // ESP_LOGI("PowerManager", "adc_values_{ %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}", 
        //          adc_values_[0], adc_values_[1], adc_values_[2], adc_values_[3], adc_values_[4], 
        //          adc_values_[5], adc_values_[6], adc_values_[7], adc_values_[8], adc_values_[9]);

        // 检测电压是否突然升高（可能刚插上充电器）
        // 如果电压从非充电范围（<4200mV）突然跳到充电范围（>4300mV），说明刚插上充电器
        uint32_t current_voltage = average_adc * 2;

        // 注意：充电状态下也会采集ADC（用于检测充电状态），但不会用ADC值更新电量
        // 因为充电时ADC读取的是充电器电压（5V），不是电池电压，电量不准确
        // 充电状态下的电量由 UpdateChargingSimulation() 根据模拟计算更新
        // 只有在非充电状态，或者充电模拟未激活时，才根据ADC值更新电量
        // 当电压超过4.3V时，说明正在充电，不更新电量（避免错误更新为100%）
        if ((!is_charging_ || !is_charging_simulation_active_) && current_voltage <= 4300) {
            CalculateBatteryLevel(current_voltage);
        }
        // if(times++ % 50 == 0){
        //     ESP_LOGI("PowerManager", "adc: %d adc_avg: %ld, VBAT: %ld, battery_level_: %u%%", 
        //         adc_value, average_adc, average_adc*2, battery_level_);
        // }
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        battery_level_ = estimate_soc(average_adc);
    }

    // 处理充电状态变化
    void HandleChargingStateChange() {
        // 检测从不充电到充电的状态变化
        if (is_charging_ && !was_charging_) {
            // 进入充电状态
            StartChargingSimulation();
        } else if (!is_charging_ && was_charging_) {
            // 退出充电状态
            StopChargingSimulation();
        }
        
        // 更新上一次的充电状态
        was_charging_ = is_charging_;
        
        // 如果正在充电，更新充电模拟
        if (is_charging_ && is_charging_simulation_active_) {
            UpdateChargingSimulation();
        }
    }

    // 开始充电模拟
    void StartChargingSimulation() {
        // 必须使用上次记录的非充电状态下的电量作为起始电量
        // 因为充电模式下ADC读取的是充电器电压（5V），电量不准确
        // 优先使用内存中的记录，如果没有则从本地存储读取
        if (last_recorded_level_ == 0) {
            last_recorded_level_ = LoadBatteryLevelFromStorage();
        }
        
        if (last_recorded_level_ == 0) {
            // 如果本地存储也没有，使用保守的默认值50%，并给出警告
            charge_start_level_ = 50;
            ESP_LOGW("PowerManager", "[充电模拟] 警告：没有非充电状态下的电量记录，使用默认值50%%作为起始电量");
            ESP_LOGW("PowerManager", "[充电模拟] 建议：先断开充电器，等待60秒让系统记录真实电量后再充电");
        } else {
            charge_start_level_ = last_recorded_level_;
            ESP_LOGI("PowerManager", "[充电模拟] 使用记录的非充电状态电量: %d%%", charge_start_level_);
        }
        
        charge_start_time_us_ = esp_timer_get_time();
        is_charging_simulation_active_ = true;
        is_fully_charged_ = false;
        
        // 计算充满所需时间（小时）
        // 需要充的电量百分比 = 100 - charge_start_level_
        // 需要充的容量(mAh) = (100 - charge_start_level_) / 100 * BATTERY_CAPACITY_MAH
        // 充满时间(小时) = 需要充的容量 / 充电电流
        // 充满时间(秒) = 充满时间(小时) * 3600
        uint32_t charge_percent_needed = 100 - charge_start_level_;
        uint32_t charge_capacity_needed_mah = (charge_percent_needed * BATTERY_CAPACITY_MAH) / 100;
        uint32_t charge_time_seconds = (charge_capacity_needed_mah * 3600) / CHARGE_CURRENT_MA;
        
        ESP_LOGI("PowerManager", "[充电模拟] 开始充电模拟 - 起始电量: %d%%, 需要充: %" PRIu32 "%%, 预计充满时间: %" PRIu32 "秒 (%.2f小时)", 
                 charge_start_level_, charge_percent_needed, charge_time_seconds, charge_time_seconds / 3600.0f);
        ESP_LOGI("PowerManager", "[充电模拟] 电池容量: %" PRIu32 "mAh, 充电电流: %" PRIu32 "mA", BATTERY_CAPACITY_MAH, CHARGE_CURRENT_MA);
    }

    // 停止充电模拟
    void StopChargingSimulation() {
        if (is_charging_simulation_active_) {
            ESP_LOGI("PowerManager", "[充电模拟] 停止充电模拟 - 当前电量: %d%%", battery_level_);
            is_charging_simulation_active_ = false;
            is_fully_charged_ = false;
        }
    }

    // 更新充电模拟
    void UpdateChargingSimulation() {
        if (is_fully_charged_) {
            return;  // 已经充满，不需要更新
        }
        
        int64_t current_time_us = esp_timer_get_time();
        int64_t elapsed_time_us = current_time_us - charge_start_time_us_;
        int64_t elapsed_time_seconds = elapsed_time_us / 1000000;
        
        // 计算已充入的电量(mAh)
        // 已充入容量 = (充电电流 * 已充电时间(小时))
        uint32_t charged_capacity_mah = (CHARGE_CURRENT_MA * elapsed_time_seconds) / 3600;
        
        // 计算当前电量百分比
        // 已充入的百分比 = (已充入容量 / 电池容量) * 100
        uint32_t charged_percent = (charged_capacity_mah * 100) / BATTERY_CAPACITY_MAH;
        uint8_t simulated_level = charge_start_level_ + charged_percent;
        
        if (simulated_level > 100) {
            simulated_level = 100;
        }
        
        // 更新电池电量（在充电状态下使用模拟值）
        battery_level_ = simulated_level;
        
        // 每10秒保存一次模拟电量到本地存储
        if (last_charge_save_time_us_ == 0 || 
            (current_time_us - last_charge_save_time_us_) >= (CHARGE_SIMULATION_SAVE_INTERVAL_MS * 1000)) {
            SaveBatteryLevelToStorage(simulated_level);
            last_charge_save_time_us_ = current_time_us;
        }
        
        // 检查是否充满
        if (simulated_level >= 100 && !is_fully_charged_) {
            is_fully_charged_ = true;
            int64_t total_charge_time_seconds = elapsed_time_seconds;
            // 充满时立即保存
            SaveBatteryLevelToStorage(100);
            ESP_LOGI("PowerManager", "[充电模拟] 电池已充满! 充电时间: %lld秒 (%.2f小时), 从 %d%% 充到 100%%", 
                     total_charge_time_seconds, total_charge_time_seconds / 3600.0f, charge_start_level_);
        }
        
        // 每10秒打印一次充电进度
        static int64_t last_log_time = 0;
        if (elapsed_time_seconds - last_log_time >= 10) {
            last_log_time = elapsed_time_seconds;
            ESP_LOGI("PowerManager", "[充电模拟] 充电进度 - 电量: %d%%, 已充电时间: %lld秒, 已充入: %" PRIu32 "mAh", 
                     simulated_level, elapsed_time_seconds, charged_capacity_mah);
        }
    }

    // 非充电状态下定期记录电量
    void RecordBatteryLevelWhenNotCharging() {
        int64_t current_time_us = esp_timer_get_time();
        if (average_adc*2 < BATTERY_FULL_VOLTAGE) {
            // 检查是否满足保存条件：间隔30秒 且 电量有变化
            bool should_save = false;
            
            // 检查时间间隔（30秒 = 30000000微秒）
            int64_t time_since_last_record = current_time_us - last_battery_record_time_us_;
            bool time_interval_ok = (last_battery_record_time_us_ == 0) || 
                                    (time_since_last_record >= 30000000);
            
            // 检查电量是否有变化
            bool level_changed = (last_recorded_level_ != battery_level_);
            
            if (time_interval_ok && level_changed) {
                should_save = true;
            }
            
            if (should_save) {
                last_recorded_level_ = battery_level_;
                last_battery_record_time_us_ = current_time_us;
                // 保存到本地存储
                SaveBatteryLevelToStorage(battery_level_);
                ESP_LOGI("PowerManager", "[电量记录] 非充电状态 - 记录电量: %d%%, 时间戳: %lld", 
                         battery_level_, current_time_us / 1000000);
            }
        }
    }
    
    // 保存电量到本地存储
    void SaveBatteryLevelToStorage(uint8_t level) {
        Settings settings("battery", true);
        settings.SetInt("last_level", level);
    }
    
    // 从本地存储读取电量
    uint8_t LoadBatteryLevelFromStorage() {
        Settings settings("battery", false);
        int32_t level = settings.GetInt("last_level", 0);
        if (level > 0 && level <= 100) {
            return static_cast<uint8_t>(level);
        }
        ESP_LOGI("PowerManager", "[存储] 本地存储中没有电量记录");
        return 0;
    }

public:
    PowerManager()
        : charging_pin_(GPIO_NUM_NC), bat_led_pin_(GPIO_NUM_NC), adc_unit_(BAT_ADC_UNIT), adc_channel_(BAT_ADC_CHANNEL) {

        // 配置充电引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);

        // 配置状态引脚
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pin_bit_mask = (1ULL << bat_led_pin_);
        gpio_config(&io_conf);

        // 定时器配置
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->CheckBatteryStatus();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 100000));  // 每100ms执行一次

        // 初始化ADC
        InitializeAdc();
        
        // 从本地存储读取上次记录的电量
        last_recorded_level_ = LoadBatteryLevelFromStorage();
        if (last_recorded_level_ > 0) {
            ESP_LOGI("PowerManager", "[初始化] 从本地存储恢复电量记录: %d%%", last_recorded_level_);
        }
        
        ESP_LOGI("PowerManager", "[初始化] PowerManager初始化完成，定时器已启动（每100ms执行一次）");
        ESP_LOGI("PowerManager", "[初始化] 充电检测阈值: >%dmV为充电, <%dmV为非充电", 
                 BATTERY_FULL_VOLTAGE, BATTERY_NOT_CHARGING_VOLTAGE);

    }

    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = adc_unit_,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };

        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &chan_config));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }
    
    bool IsFullyCharged() { return is_fully_charged_; }
    
    // 立即检测一次电量
    void CheckBatteryStatusImmediately() {
        CheckBatteryStatus();
    }

    static PowerManager& GetInstance() {
        static PowerManager instance; // 使用默认构造函数初始化对象
        return instance;
    }

    void EnterDeepSleepIfNotCharging() {
        // 不在充电就真休眠
        ESP_LOGI("PowerManager", "EnterDeepSleepIfNotCharging");
        if (is_charging_) {
            // 充电中，只断开 socket
            Application::GetInstance().QuitTalking();
            return;
        }
        vb6824_shutdown();
        vTaskDelay(pdMS_TO_TICKS(200));
        // 配置唤醒源 只有电源域是VDD3P3_RTC的才能唤醒深睡
        uint64_t wakeup_pins = (BIT(GPIO_NUM_1));
        esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
        ESP_LOGI("PowerMgr", "ready to esp_deep_sleep_start");
        vTaskDelay(pdMS_TO_TICKS(10));
        
        esp_deep_sleep_start();
}

    
};
#endif  // __POWER_MANAGER_H__
