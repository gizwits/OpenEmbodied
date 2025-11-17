#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <functional>
#include "config.h"  // 包含GPIO引脚配置和ADC配置

class PowerManager {
public:
    // 充电检测配置参数
    static constexpr uint32_t CHARGING_DETECT_VOLTAGE_THRESHOLD_MV = 4400;  // VDD电压阈值，大于此值表示充电中

private:
    // 可配置的电池分压/缩放系数：VBAT = calibrated_mV * VBAT_SCALE_NUM / VBAT_SCALE_DEN
    // 参考gizwits-c2-6824-DRF-W300CA项目，分压比为2:1
    static constexpr int VBAT_SCALE_NUM = 2;
    static constexpr int VBAT_SCALE_DEN = 1;
    
    // 电压-SOC对照表（阶梯式显示：0,10,20,30,40,50,60,70,80,90,100）
    // 参考gizwits-c2-6824-DRF-W300CA项目
    static constexpr struct VoltageSocPair {
        uint16_t voltage_mv; // 电压值(mV)
        uint8_t soc;         // 电量百分比
    } dischargeCurve[] = {
        {4140, 100}, {4104, 95}, {4068, 90}, {4032, 85}, {3996, 80},
        {3960, 75}, {3924, 70}, {3888, 65}, {3852, 60}, {3829, 55},
        {3808, 50}, {3787, 45}, {3766, 40}, {3745, 35}, {3724, 30},
        {3703, 25}, {3672, 20}, {3570, 15}, {3420, 10}, {3220, 1}
    };
    
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    gpio_num_t charging_pin_;
    gpio_num_t bat_led_pin_;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    bool is_charging_ = false;

    static constexpr uint8_t MAX_CHANGE_COUNT = 8;
    static constexpr uint32_t TIME_LIMIT = 2000000; // 2 seconds in microseconds

    uint8_t change_count_ = 0;  // 记录状态变化次数
    uint64_t last_change_time_ = 0;  // 最后一次状态变化的时间戳（微秒）

    adc_oneshot_unit_handle_t adc_handle_;  // 保留旧接口，用于兼容

    // 电池电压检测ADC句柄（使用ADC2_CH1检测VBAT电压）
    adc_oneshot_unit_handle_t battery_adc_handle_ = nullptr;
    bool battery_adc_initialized_ = false;
    int battery_adc_value_ = 0;  // 三次采集平均值
    uint32_t average_adc_ = 0;   // 滑动窗口平均值（用于电压计算）
    
    // ADC校准句柄
    adc_cali_handle_t cali_handle_ = nullptr;
    bool cali_inited_ = false;

    // 充电检测ADC句柄（使用ADC2_CH2检测VDD电压）
    adc_oneshot_unit_handle_t charging_detect_adc_handle_ = nullptr;
    bool charging_detect_adc_initialized_ = false;
    
    // VDD电压检测相关（仅用于判断充电状态）
    int charging_adc_value_ = 0;  // 三次采集平均值
    
    // 充电状态防抖相关
    bool charging_state_candidate_ = false;  // 候选充电状态
    uint8_t charging_state_count_ = 0;  // 连续检测到候选状态的次数
    static constexpr uint8_t CHARGING_STATE_DEBOUNCE_COUNT = 5;  // 需要连续5次检测到相同状态才切换

    // 充电状态改变回调函数
    std::function<void(bool)> charging_status_callback_ = nullptr;

    void CheckBatteryStatus() {
        // 先读取电池电压ADC数据（使用ADC2_CH1）
        if (battery_adc_initialized_) {
            ReadBatteryAdcData();
            // 根据电压计算电量（使用电压-SOC对照表）
            CalculateBatteryLevel();
        }
        
        // 再读取充电检测ADC数据（使用ADC2_CH2检测VDD电压）
        if (charging_detect_adc_initialized_) {
            ReadChargingAdcData();
            CheckChargingStatus();
        }
        
        // 每30秒打印一次（采样间隔100ms，30秒=300次采样）
        static uint32_t print_counter = 0;
        print_counter++;
        if (print_counter >= 300) {
            print_counter = 0;
            
            // 打印两个ADC的平均值、电量、充电状态
            if (battery_adc_initialized_ && charging_detect_adc_initialized_) {
                ESP_LOGI("PowerManager", "🔋 ADC平均值: 电池(ADC2_CH1)=%d, 充电检测(ADC2_CH2)=%d, 电量=%d%%, 充电状态=%s", 
                         average_adc_, charging_adc_value_, battery_level_, is_charging_ ? "充电中" : "未充电");
            } else if (battery_adc_initialized_) {
                ESP_LOGI("PowerManager", "🔋 ADC平均值: 电池(ADC2_CH1)=%d, 电量=%d%%, 充电状态=%s (充电检测ADC未初始化)", 
                         average_adc_, battery_level_, is_charging_ ? "充电中" : "未充电");
            } else if (charging_detect_adc_initialized_) {
                ESP_LOGI("PowerManager", "🔋 ADC平均值: 充电检测(ADC2_CH2)=%d, 充电状态=%s (电池ADC未初始化)", 
                         charging_adc_value_, is_charging_ ? "充电中" : "未充电");
            } else {
                // 如果两个ADC都未初始化，打印警告
                ESP_LOGW("PowerManager", "🔋 ADC值: 两个ADC都未初始化 (电池ADC初始化=%d, 充电检测ADC初始化=%d)", 
                         battery_adc_initialized_, charging_detect_adc_initialized_);
            }
        }
    }
    
    // 读取电池电压ADC数据（三次采集取平均值，与充电检测ADC方式一致）
    void ReadBatteryAdcData() {
        // 检查ADC句柄是否有效
        if (battery_adc_handle_ == nullptr) {
            ESP_LOGW("PowerManager", "🔋 电池ADC句柄未设置，跳过读取");
            return;
        }
        
        // 三次采集取平均值
        int32_t sum = 0;
        int32_t valid_reads = 0;
        
        for (int i = 0; i < 3; i++) {
            int temp_value = 0;
            esp_err_t ret = adc_oneshot_read(battery_adc_handle_, BAT_ADC_CHANNEL, &temp_value);
            if (ret == ESP_OK) {
                sum += temp_value;
                valid_reads++;
            } else {
                ESP_LOGW("PowerManager", "🔋 电池ADC第%d次读取失败: %s", i+1, esp_err_to_name(ret));
            }
        }
        
        if (valid_reads == 0) {
            ESP_LOGW("PowerManager", "🔋 电池ADC三次读取全部失败");
            return;
        }
        
        // 计算三次采集的平均值
        battery_adc_value_ = sum / valid_reads;
        
        ESP_LOGD("PowerManager", "🔋 电池ADC三次采集平均值: %d", battery_adc_value_);
        
        // 使用平均值更新滑动窗口（参考gizwits-c2-6824-DRF-W300CA项目）
        // 首次读取时预填充均值缓冲
        if (adc_values_count_ == 0) {
            for (size_t i = 0; i < ADC_VALUES_COUNT; ++i) {
                adc_values_[i] = battery_adc_value_;
            }
            adc_values_count_ = ADC_VALUES_COUNT;
            adc_values_index_ = 1 % ADC_VALUES_COUNT;
        } else {
            adc_values_[adc_values_index_] = battery_adc_value_;
            adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
            if (adc_values_count_ < ADC_VALUES_COUNT) {
                adc_values_count_++;
            }
        }
        
        // 计算滑动窗口平均值
        average_adc_ = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc_ += adc_values_[i];
        }
        average_adc_ /= adc_values_count_;
        
        ESP_LOGD("PowerManager", "🔋 ADC滤波: 单次=%d, 滑动平均=%d", battery_adc_value_, average_adc_);
    }
    
    // 读取充电检测ADC数据（三次采集取平均值）
    void ReadChargingAdcData() {
        // 检查ADC句柄是否有效
        if (charging_detect_adc_handle_ == nullptr) {
            ESP_LOGW("PowerManager", "🔋 充电检测ADC句柄未设置，跳过读取");
            return;
        }
        
        // 三次采集取平均值
        int32_t sum = 0;
        int32_t valid_reads = 0;
        
        for (int i = 0; i < 3; i++) {
            int temp_value = 0;
            esp_err_t ret = adc_oneshot_read(charging_detect_adc_handle_, CHARGING_DETECT_ADC_CHANNEL, &temp_value);
            if (ret == ESP_OK) {
                sum += temp_value;
                valid_reads++;
            } else {
                ESP_LOGW("PowerManager", "🔋 充电检测ADC第%d次读取失败: %s", i+1, esp_err_to_name(ret));
            }
        }
        
        if (valid_reads == 0) {
            ESP_LOGW("PowerManager", "🔋 充电检测ADC三次读取全部失败");
            return;
        }
        
        // 计算三次采集的平均值
        charging_adc_value_ = sum / valid_reads;
        
        ESP_LOGD("PowerManager", "🔋 充电检测ADC三次采集平均值: %d", charging_adc_value_);
    }
    
    // 检查充电状态（仅判断充电/未充电，不计算电量）
    void CheckChargingStatus() {
        // ADC值已在CheckBatteryStatus中统一打印，这里不再单独打印
        
        bool previous_charging = is_charging_;
        
        // 判断充电状态：使用ADC2_CH2的值
        // ADC值 > 3000 -> 未充电（电池供电）
        // ADC值 <= 3000 -> 充电中（USB供电）
        const int CHARGING_ADC_THRESHOLD = 3000;
        bool candidate_charging = false;
        if (charging_adc_value_ > CHARGING_ADC_THRESHOLD) {
            // ADC值高，表示未充电（电池供电）
            candidate_charging = false;
        } else {
            // ADC值低，表示充电中（USB供电）
            candidate_charging = true;
        }
        
        // 防抖机制：需要连续多次检测到相同状态才切换
        // 只有当候选状态与当前实际状态不同时，才进行防抖计数
        if (candidate_charging != is_charging_) {
            // 候选状态与当前实际状态不同，需要防抖
            if (candidate_charging == charging_state_candidate_) {
                // 候选状态与之前相同，增加计数（但不超过防抖次数）
                if (charging_state_count_ < CHARGING_STATE_DEBOUNCE_COUNT) {
                    charging_state_count_++;
                }
            } else {
                // 候选状态改变，重置计数
                charging_state_candidate_ = candidate_charging;
                charging_state_count_ = 1;
            }
            
            // 只有连续检测到相同状态达到防抖次数，才真正切换状态
            if (charging_state_count_ >= CHARGING_STATE_DEBOUNCE_COUNT) {
                is_charging_ = charging_state_candidate_;
                ESP_LOGI("PowerManager", "🔋 充电状态变化: %s -> %s (ADC2_CH2值: %d, 阈值: %d, 连续检测%d次)", 
                         previous_charging ? "充电中" : "未充电", 
                         is_charging_ ? "充电中" : "未充电", 
                         charging_adc_value_, CHARGING_ADC_THRESHOLD, charging_state_count_);
                if (charging_status_callback_) {
                    charging_status_callback_(is_charging_);
                }
            }
        } else {
            // 候选状态与当前实际状态相同，重置防抖计数（状态已稳定）
            charging_state_candidate_ = candidate_charging;
            charging_state_count_ = 0;
        }
    }

    // 查表函数 - 根据电压估算SOC（参考gizwits-c2-6824-DRF-W300CA项目）
    uint8_t estimate_soc_from_voltage(uint16_t voltage_mv) {
        uint16_t closest_voltage = dischargeCurve[0].voltage_mv;
        uint8_t closest_soc = dischargeCurve[0].soc;
        uint16_t min_diff = abs((int)voltage_mv - (int)closest_voltage);

        for (size_t i = 1; i < sizeof(dischargeCurve) / sizeof(dischargeCurve[0]); i++) {
            uint16_t diff = abs((int)voltage_mv - (int)dischargeCurve[i].voltage_mv);
            if (diff < min_diff) {
                min_diff = diff;
                closest_voltage = dischargeCurve[i].voltage_mv;
                closest_soc = dischargeCurve[i].soc;
            }
        }
        return closest_soc;
    }
    
    // 获取电池电压（带分压计算，参考gizwits-c2-6824-DRF-W300CA项目）
    uint32_t GetBatteryVoltage() {
        // 计算经校准后的电压（mV），并换算到VBAT
        int mv = average_adc_;
        if (cali_inited_ && cali_handle_ != nullptr) {
            (void)adc_cali_raw_to_voltage(cali_handle_, average_adc_, &mv);
        } else {
            // 如果没有校准，使用原始换算（12位ADC，12dB衰减，最大3.3V）
            // ADC值范围0-4095，对应0-3300mV
            mv = (average_adc_ * 3300) / 4095;
        }
        
        // 应用分压系数：VBAT = calibrated_mV * VBAT_SCALE_NUM / VBAT_SCALE_DEN
        uint32_t voltage = (uint32_t)((int64_t)mv * VBAT_SCALE_NUM / VBAT_SCALE_DEN);
        
        return voltage;
    }
    
    // 根据电压计算电量（使用电压-SOC对照表）
    void CalculateBatteryLevel() {
        uint32_t voltage_mv = GetBatteryVoltage();
        battery_level_ = estimate_soc_from_voltage((uint16_t)voltage_mv);
    }
    
    // 初始化电池电压检测ADC（使用ADC2_CH1）
    void InitializeBatteryAdc() {
        ESP_LOGI("PowerManager", "开始初始化电池ADC: unit=%d, channel=%d, atten=%d", 
                 BAT_ADC_UNIT, BAT_ADC_CHANNEL, BAT_ADC_ATTEN);
        
        // 如果充电检测ADC已经初始化，且使用同一单元，则共享句柄
        if (BAT_ADC_UNIT == CHARGING_DETECT_ADC_UNIT && charging_detect_adc_handle_ != nullptr) {
            ESP_LOGI("PowerManager", "电池ADC与充电检测ADC使用同一单元，共享句柄");
            battery_adc_handle_ = charging_detect_adc_handle_;
        } else {
            // 创建新的ADC单元句柄
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = BAT_ADC_UNIT,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            esp_err_t ret = adc_oneshot_new_unit(&init_config, &battery_adc_handle_);
            if (ret != ESP_OK) {
                ESP_LOGE("PowerManager", "创建电池ADC单元失败: %s (0x%x)", esp_err_to_name(ret), ret);
                battery_adc_handle_ = nullptr;
                return;
            }
        }
        
        // 配置通道（无论是否共享句柄，都需要配置通道）
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_12,
        };
        esp_err_t ret = adc_oneshot_config_channel(battery_adc_handle_, BAT_ADC_CHANNEL, &chan_config);
        if (ret != ESP_OK) {
            ESP_LOGE("PowerManager", "配置电池ADC通道失败: %s (0x%x)", esp_err_to_name(ret), ret);
            // 只有在创建了新单元时才删除
            if (BAT_ADC_UNIT != CHARGING_DETECT_ADC_UNIT || charging_detect_adc_handle_ == nullptr) {
                adc_oneshot_del_unit(battery_adc_handle_);
            }
            battery_adc_handle_ = nullptr;
            return;
        }
        
        battery_adc_initialized_ = true;
        ESP_LOGI("PowerManager", "电池ADC2_CH1初始化成功，用于检测VBAT电压");
        
        // 创建ADC校准（曲线拟合方案，ESP32-S3使用curve_fitting）
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = BAT_ADC_UNIT,
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle_) == ESP_OK) {
            cali_inited_ = true;
            ESP_LOGI("PowerManager", "ADC校准已启用（curve fitting）");
        } else {
            cali_inited_ = false;
            cali_handle_ = nullptr;
            ESP_LOGW("PowerManager", "ADC校准不可用，改用原始raw换算");
        }
    }
    
    // 初始化充电检测ADC
    void InitializeChargingDetectAdc() {
        ESP_LOGI("PowerManager", "开始初始化充电检测ADC: unit=%d, channel=%d, atten=%d", 
                 CHARGING_DETECT_ADC_UNIT, CHARGING_DETECT_ADC_CHANNEL, CHARGING_DETECT_ADC_ATTEN);
        
        // 如果电池ADC已经初始化，且使用同一单元，则共享句柄
        if (CHARGING_DETECT_ADC_UNIT == BAT_ADC_UNIT && battery_adc_handle_ != nullptr) {
            ESP_LOGI("PowerManager", "充电检测ADC与电池ADC使用同一单元，共享句柄");
            charging_detect_adc_handle_ = battery_adc_handle_;
        } else {
            // 创建新的ADC单元句柄
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = CHARGING_DETECT_ADC_UNIT,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            esp_err_t ret = adc_oneshot_new_unit(&init_config, &charging_detect_adc_handle_);
            if (ret != ESP_OK) {
                ESP_LOGE("PowerManager", "创建充电检测ADC单元失败: %s (0x%x)", esp_err_to_name(ret), ret);
                charging_detect_adc_handle_ = nullptr;
                return;
            }
        }
        
        // 配置通道（无论是否共享句柄，都需要配置通道）
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = CHARGING_DETECT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_12,
        };
        esp_err_t ret = adc_oneshot_config_channel(charging_detect_adc_handle_, CHARGING_DETECT_ADC_CHANNEL, &chan_config);
        if (ret != ESP_OK) {
            ESP_LOGE("PowerManager", "配置充电检测ADC通道失败: %s (0x%x)", esp_err_to_name(ret), ret);
            // 只有在创建了新单元时才删除
            if (CHARGING_DETECT_ADC_UNIT != BAT_ADC_UNIT || battery_adc_handle_ == nullptr) {
                adc_oneshot_del_unit(charging_detect_adc_handle_);
            }
            charging_detect_adc_handle_ = nullptr;
            return;
        }
        
        charging_detect_adc_initialized_ = true;
        ESP_LOGI("PowerManager", "充电检测ADC2_CH2初始化成功，用于检测VDD电压");
    }
    

public:
    PowerManager(gpio_num_t charging_pin, gpio_num_t bat_led_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3)
        : charging_pin_(charging_pin), bat_led_pin_(bat_led_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {

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
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 100000));  // 100ms采样（参考gizwits-c2-6824-DRF-W300CA项目）

        // 初始化电池电压检测ADC（使用ADC2_CH1）
        ESP_LOGI("PowerManager", "准备初始化电池ADC...");
        InitializeBatteryAdc();
        ESP_LOGI("PowerManager", "电池ADC初始化完成，状态: %d", battery_adc_initialized_);
        
        // 初始化充电检测ADC（使用ADC2_CH2）
        ESP_LOGI("PowerManager", "准备初始化充电检测ADC...");
        InitializeChargingDetectAdc();
        ESP_LOGI("PowerManager", "充电检测ADC初始化完成，状态: %d", charging_detect_adc_initialized_);
        
        // 打印最终初始化状态
        if (battery_adc_initialized_ && charging_detect_adc_initialized_) {
            ESP_LOGI("PowerManager", "✅ 两个ADC初始化成功");
        } else {
            ESP_LOGW("PowerManager", "⚠️ ADC初始化状态: 电池ADC=%d, 充电检测ADC=%d", 
                     battery_adc_initialized_, charging_detect_adc_initialized_);
        }
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
        // 清理电池ADC句柄（只有在创建了独立单元时才删除）
        if (battery_adc_handle_ && (BAT_ADC_UNIT != CHARGING_DETECT_ADC_UNIT || charging_detect_adc_handle_ == nullptr)) {
            adc_oneshot_del_unit(battery_adc_handle_);
        }
        // 清理充电检测ADC句柄（只有在创建了独立单元时才删除）
        if (charging_detect_adc_handle_ && (CHARGING_DETECT_ADC_UNIT != BAT_ADC_UNIT || battery_adc_handle_ == nullptr)) {
            adc_oneshot_del_unit(charging_detect_adc_handle_);
        }
        // 清理ADC校准句柄（ESP32-S3使用curve_fitting）
        if (cali_inited_ && cali_handle_) {
            adc_cali_delete_scheme_curve_fitting(cali_handle_);
            cali_handle_ = nullptr;
            cali_inited_ = false;
        }
    }

    bool IsCharging() { return is_charging_; }
    
    // 检测充电状态（使用ADC2_CH2检测VDD电压）
    bool IsChargingByVdd() {
        return is_charging_;
    }

    uint8_t GetBatteryLevel() { return battery_level_; }
    
    // 立即检测一次电量
    void CheckBatteryStatusImmediately() {
        CheckBatteryStatus();
    }

    // 设置充电状态改变回调函数
    void SetChargingStatusCallback(std::function<void(bool)> callback) {
        charging_status_callback_ = callback;
    }
};
#endif  // __POWER_MANAGER_H__
