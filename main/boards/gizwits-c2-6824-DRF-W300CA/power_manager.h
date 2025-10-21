#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "vb6824.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include "config.h"
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

// Battery ADC configuration
#define BAT_ADC_CHANNEL  ADC_CHANNEL_3  // Battery voltage ADC channel
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_12 // ADC attenuation
#define BAT_ADC_UNIT     ADC_UNIT_1
#define POWER_CHARGE_LED_PIN GPIO_NUM_NC

// 可配置的电池分压/缩放系数：VBAT = calibrated_mV * VBAT_SCALE_NUM / VBAT_SCALE_DEN
#define VBAT_SCALE_NUM   2
#define VBAT_SCALE_DEN   1

// 系统基础电流配置
#define SYSTEM_BASE_CURRENT_MA 5.0         // 系统基础电流5mA

// 负载压降补偿配置（根据规格书调整）
#define MAX_MOTOR_CURRENT_MA 50.0          // 电机最大电流50mA
#define MAX_LED_CURRENT_MA 400.0           // LED最大电流400mA
#define BATTERY_INTERNAL_RESISTANCE_MOHM 300.0  // 电池内阻300mΩ（进一步增加）
#define LINE_RESISTANCE_MOHM 150.0         // 线路阻抗150mΩ（进一步增加）
#define PWM_INTERFERENCE_COMPENSATION_MV 240  // PWM干扰补偿240mV（微调-10mV）

class PowerManager {
private:
    static constexpr size_t ADC_VALUES_COUNT = 10;

    esp_timer_handle_t timer_handle_ = nullptr;
    adc_unit_t adc_unit_;
    adc_channel_t adc_channel_;
    int adc_value;
    uint16_t adc_values_[ADC_VALUES_COUNT];
    size_t adc_values_index_ = 0;
    size_t adc_values_count_ = 0;
    uint8_t battery_level_ = 100;
    uint32_t average_adc = 0;
    bool is_charging_ = false;

    adc_oneshot_unit_handle_t adc_handle_;
    adc_cali_handle_t cali_handle_ = nullptr;
    bool cali_inited_ = false;

    
    // 系统状态变量（需要从外部更新）
    bool motor_running_ = false;                            // 电机运行状态
    bool led_enabled_ = false;                              // LED使能状态
    uint8_t motor_speed_ = 0;                               // 电机速度 0-100
    uint8_t led_brightness_ = 0;                            // LED亮度 0-100
    uint8_t led_mode_ = 0;                                  // LED模式 0-5
    uint8_t baseline_soc_ = 100;                            // 无负载参考SOC
    bool baseline_valid_ = false;                           // 基线是否有效
    uint32_t last_no_load_time_ms_ = 0;                    // 最近无负载开始时间
    // 关机判定稳定窗口/去抖
    uint32_t last_load_change_time_ms_ = 0;                // 最近一次负载变化时间
    uint32_t ignore_shutdown_until_ms_ = 0;                // 在该时间点之前跳过关机判定
    uint8_t shutdown_below_count_ = 0;                     // 连续低于阈值的计数
    uint32_t last_voltage_mv_ = 0;                         // 最近一次用于判定的电压
    bool shutdown_checks_enabled_ = false;                  // 是否允许执行关机判定
    bool startup_check_pending_ = false;                    // 开机最终判定是否待执行
    uint32_t startup_check_after_ms_ = 0;                   // 开机最终判定的起始时间点
    
    
    // 电量平滑过渡相关变量
    uint8_t displayed_battery_level_ = 100;                 // 显示给用户的电量
    uint8_t target_battery_level_ = 100;                    // 目标电量
    uint32_t last_smooth_update_time_ = 0;                  // 上次平滑更新时间
    static constexpr uint32_t SMOOTH_UPDATE_INTERVAL_MS = 200;  // 平滑更新间隔200ms
    static constexpr uint8_t MAX_SMOOTH_STEP = 2;           // 每次最大变化2%
    
    // 补偿状态变量
    bool is_compensating = false;                           // 是否正在补偿
    uint32_t last_compensation_mv_ = 0;                     // 最近一次补偿绝对值(mV)
    // 低电/关机阈值（根据需求）
    static constexpr uint8_t LOW_BATTERY_SOC = 10;          // 低电量播报：≤10%
    static constexpr uint8_t LOW_BATTERY_SOC_HYS = 2;       // 回滞：≥12%恢复
    static constexpr uint16_t SHUTDOWN_CUTOFF_MV = 3470;    // 放电截止：3.4V（关机）
    static constexpr uint16_t LOW_BATTERY_VOLTAGE_MV = 3500; // 低电量播报：3.45V
    bool low_battery_ = false;                               // 低电量状态

    // 电压-SOC对照表（阶梯式显示：0,10,20,30,40,50,60,70,80,90,100）
    static constexpr struct VoltageSocPair {
        uint16_t voltage_mv; // 电压值(mV)
        uint8_t soc;         // 电量百分比
    } dischargeCurve[] = {
        {4200, 100}, // 充满电电压
        {4100, 90},  // 90%
        {4000, 80},  // 80%
        {3900, 70},  // 70%
        {3800, 60},  // 60%
        {3700, 50},  // 50%
        {3600, 40},  // 40%
        {3550, 30},  // 30%
        {3500, 20},  // 20%
        {3450, 10},  // 10%
        {3400, 0}    // 0% - 放电截止关机
    };

    // 查表函数 - 根据电压估算SOC
    uint8_t estimate_soc_from_voltage(uint16_t voltage_mv) {
        uint16_t closest_voltage = dischargeCurve[0].voltage_mv;
        uint8_t closest_soc = dischargeCurve[0].soc;
        uint16_t min_diff = abs(voltage_mv - closest_voltage);

        for (size_t i = 1; i < sizeof(dischargeCurve) / sizeof(dischargeCurve[0]); i++) {
            uint16_t diff = abs(voltage_mv - dischargeCurve[i].voltage_mv);
            if (diff < min_diff) {
                min_diff = diff;
                closest_voltage = dischargeCurve[i].voltage_mv;
                closest_soc = dischargeCurve[i].soc;
            }
        }
        return closest_soc;
    }

    // 反查表函数 - 根据SOC获取目标电压(就近匹配阶梯)
    uint16_t voltage_for_soc(uint8_t target_soc) {
        uint16_t best_voltage = dischargeCurve[0].voltage_mv;
        uint32_t best_diff = 1000;
        for (size_t i = 0; i < sizeof(dischargeCurve) / sizeof(dischargeCurve[0]); i++) {
            uint32_t diff = (uint32_t)abs((int)dischargeCurve[i].soc - (int)target_soc);
            if (diff < best_diff) {
                best_diff = diff;
                best_voltage = dischargeCurve[i].voltage_mv;
            }
        }
        return best_voltage;
    }

    
    // 获取最大负载电流（用于固定补偿计算）
    float GetMaxLoadCurrent() {
        // 基于规格书的最大负载电流
        return MAX_MOTOR_CURRENT_MA + MAX_LED_CURRENT_MA + SYSTEM_BASE_CURRENT_MA;
    }
    
    // 电量平滑过渡更新
    void UpdateSmoothBatteryLevel() {
        uint32_t current_time = esp_timer_get_time() / 1000; // ms
        
        // 检查是否需要更新
        if (current_time - last_smooth_update_time_ < SMOOTH_UPDATE_INTERVAL_MS) {
            return;
        }
        
        last_smooth_update_time_ = current_time;
        
        // 如果目标电量和显示电量不同，进行平滑过渡
        if (displayed_battery_level_ != target_battery_level_) {
            int8_t diff = (int8_t)target_battery_level_ - (int8_t)displayed_battery_level_;
            
            if (abs(diff) <= MAX_SMOOTH_STEP) {
                // 差异小于等于最大步长，直接设置
                displayed_battery_level_ = target_battery_level_;
            } else {
                // 差异大于最大步长，逐步接近
                if (diff > 0) {
                    displayed_battery_level_ += MAX_SMOOTH_STEP;
                } else {
                    displayed_battery_level_ -= MAX_SMOOTH_STEP;
                }
            }
            
            ESP_LOGD("PowerManager", "🔋 电量平滑过渡: %d%% -> %d%% (目标: %d%%)", 
                     displayed_battery_level_, target_battery_level_, target_battery_level_);
        }
    }
    
    
    
    // 获取电池电压（带固定负载补偿）
    uint32_t GetBatteryVoltage() {
        // 计算经校准后的电压（mV），并换算到VBAT
        int mv = average_adc;
        if (cali_inited_) {
            (void)adc_cali_raw_to_voltage(cali_handle_, average_adc, &mv);
        }
        uint32_t voltage = (uint32_t)((int64_t)mv * VBAT_SCALE_NUM / VBAT_SCALE_DEN);
        
        // 动态负载补偿：根据实际负载状态计算补偿值
        // 目标：与无负载参考相比，显示电量波动≤10%
        uint32_t compensation_mv = 0;
        
        if (motor_running_ || led_enabled_) {
            // 基于实际负载电流计算补偿值
            float motor_current_ma = (motor_speed_ / 100.0f) * MAX_MOTOR_CURRENT_MA;
            float led_current_ma = (led_brightness_ / 100.0f) * MAX_LED_CURRENT_MA;
            float actual_current_ma = motor_current_ma + led_current_ma + SYSTEM_BASE_CURRENT_MA;
            
            float total_resistance_mohm = BATTERY_INTERNAL_RESISTANCE_MOHM + LINE_RESISTANCE_MOHM;
            float voltage_drop_mv = (actual_current_ma * total_resistance_mohm) / 1000.0f;
            
            // 基础补偿（微调-10mV）
            compensation_mv = (uint32_t)voltage_drop_mv + 90; // 基础额外补偿90mV
            
            // 模式1（白色）功率较大，增加额外补偿
            if (led_mode_ == 1) {
                compensation_mv += 90; // 白色模式额外90mV（微调-10mV）
            }
            
            // 高亮度时增加额外补偿
            if (led_brightness_ > 80) {
                compensation_mv += 40; // 高亮度额外40mV（微调-10mV）
            }
            
            
            // 有负载时：ADC读数偏低（负载压降），需要加上补偿值来恢复真实电压
            voltage += compensation_mv;
            is_compensating = true;
            last_compensation_mv_ = compensation_mv;
            
            ESP_LOGD("PowerManager", "🔋 负载补偿: 启用, 补偿值: +%" PRIu32 "mV (电机:%.1fmA, LED:%.1fmA, 模式:%d, 总电流:%.1fmA, 压降:%.1fmV)",
                     compensation_mv, motor_current_ma, led_current_ma, led_mode_, actual_current_ma, voltage_drop_mv);

            // 将负载下的SOC限制在无负载基线±10%以内（若基线有效）
            if (baseline_valid_) {
                uint8_t current_soc = estimate_soc_from_voltage((uint16_t)voltage);
                int16_t diff = (int16_t)current_soc - (int16_t)baseline_soc_;
                if (diff > 3) {
                    uint8_t target_soc = (uint8_t)std::min(100, (int)baseline_soc_ + 3);
                    uint16_t target_v = voltage_for_soc(target_soc);
                    int32_t delta_v = (int32_t)target_v - (int32_t)voltage;
                    voltage = (uint32_t)((int32_t)voltage + delta_v);
                    ESP_LOGD("PowerManager", "🔋 SOC上限钳位: %d%%→%d%%, 电压调整 +%" PRId32 " mV", current_soc, target_soc, delta_v);
                } else if (diff < -7) { // 下限稍宽，避免误触发
                    int target_soc_int = (int)baseline_soc_ - 7;
                    if (target_soc_int < 0) target_soc_int = 0;
                    uint8_t target_soc = (uint8_t)target_soc_int;
                    uint16_t target_v = voltage_for_soc(target_soc);
                    int32_t delta_v = (int32_t)target_v - (int32_t)voltage;
                    voltage = (uint32_t)((int32_t)voltage + delta_v);
                    ESP_LOGD("PowerManager", "🔋 SOC下限钳位: %d%%→%d%%, 电压调整 +%" PRId32 " mV", current_soc, target_soc, delta_v);
                }
            }
        } else {
            // 无负载时：电压读数正常，不需要补偿
            compensation_mv = 0;
            is_compensating = false;
            last_compensation_mv_ = 0;
            ESP_LOGD("PowerManager", "🔋 负载补偿: 禁用, 补偿值: 0mV");
        }
        
        return voltage;
    }
    
    // 检查充电状态
    void CheckChargingStatus() {
        uint32_t voltage = GetBatteryVoltage();
        
        #define BATTERY_FULL_VOLTAGE 4200      // 根据规格书4.20V
        #define BATTERY_NOT_CHARGING_VOLTAGE 4100
        
        static uint8_t not_charging_count = 0;
        bool previous_charging = is_charging_;
        
        if (voltage > BATTERY_FULL_VOLTAGE) {
            is_charging_ = true;
            not_charging_count = 0;
        } else if (voltage < BATTERY_NOT_CHARGING_VOLTAGE) {
            if (is_charging_) {
                not_charging_count++;
                if (not_charging_count >= 20) { // 2秒确认
                    is_charging_ = false;
                    not_charging_count = 0;
                }
            }
        }
        
        // 充电状态变化时立即打印
        if (previous_charging != is_charging_) {
            ESP_LOGI("PowerManager", "🔋 充电状态变化: %s -> %s (电压: %" PRIu32 "mV)", 
                     previous_charging ? "充电中" : "未充电", 
                     is_charging_ ? "充电中" : "未充电", 
                     voltage);
        }
    }


    void ReadBatteryAdcData() {
        // 检查ADC句柄是否有效
        if (adc_handle_ == nullptr) {
            ESP_LOGW("PowerManager", "🔋 ADC句柄未设置，跳过读取");
            return;
        }
        
        // 三次采集取平均值
        int32_t sum = 0;
        int32_t valid_reads = 0;
        
        for (int i = 0; i < 3; i++) {
            int temp_value = 0;
            esp_err_t ret = adc_oneshot_read(adc_handle_, adc_channel_, &temp_value);
            if (ret == ESP_OK) {
                sum += temp_value;
                valid_reads++;
            } else {
                ESP_LOGW("PowerManager", "🔋 ADC第%d次读取失败: %s", i+1, esp_err_to_name(ret));
            }
        }
        
        if (valid_reads == 0) {
            ESP_LOGW("PowerManager", "🔋 三次ADC读取全部失败");
            return;
        }
        
        // 计算三次采集的平均值
        adc_value = sum / valid_reads;
        
        ESP_LOGD("PowerManager", "🔋 ADC三次采集: %d, %d, %d -> 平均: %d", 
                 (int)(sum - adc_value * (valid_reads - 1)), 
                 (int)(adc_value), (int)(adc_value), (int)adc_value);

        // 首次读取时预填充均值缓冲
        if (adc_values_count_ == 0) {
            for (size_t i = 0; i < ADC_VALUES_COUNT; ++i) {
                adc_values_[i] = adc_value;
            }
            adc_values_count_ = ADC_VALUES_COUNT;
            adc_values_index_ = 1 % ADC_VALUES_COUNT;
        } else {
            adc_values_[adc_values_index_] = adc_value;
            adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
            if (adc_values_count_ < ADC_VALUES_COUNT) {
                adc_values_count_++;
            }
        }
        
        // 计算滑动窗口平均值
        average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];
        }
        average_adc /= adc_values_count_;
        
        ESP_LOGD("PowerManager", "🔋 ADC滤波: 单次=%d, 滑动平均=%d", (int)adc_value, (int)average_adc);
    }

public:
    PowerManager()
        : adc_unit_(BAT_ADC_UNIT), adc_channel_(BAT_ADC_CHANNEL) {

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
        // 定时器将在ADC句柄设置后启动

        // 初始化ADC
        InitializeAdc();
    }

    void InitializeAdc() {
        // ADC 单元由 CustomBoard 创建，这里只配置通道
        ESP_LOGI("PowerManager", "ADC 单元由 CustomBoard 管理，跳过创建");
        // 创建标定（线性拟合方案）
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = BAT_ADC_UNIT,
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle_) == ESP_OK) {
            cali_inited_ = true;
            ESP_LOGI("PowerManager", "ADC 标定已启用（line fitting）");
        } else {
            cali_inited_ = false;
            cali_handle_ = nullptr;
            ESP_LOGW("PowerManager", "ADC 标定不可用，改用原始raw换算");
        }
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
        if (cali_inited_ && cali_handle_) {
            adc_cali_delete_scheme_line_fitting(cali_handle_);
            cali_handle_ = nullptr;
            cali_inited_ = false;
        }
    }


    void CheckBatteryStatus() {
        // 在电机和RGB工作时延迟采样，避免PWM干扰
        static uint32_t last_motor_rgb_time = 0;
        uint32_t current_time = esp_timer_get_time() / 1000; // ms
        
        // 如果电机或RGB刚工作过，延迟采样
        if (current_time - last_motor_rgb_time < 500) { // 增加到500ms延迟
            return;
        }
        
        // 更新电机和RGB工作时间记录
        if (motor_running_ || led_enabled_) {
            last_motor_rgb_time = current_time;
        }

        ReadBatteryAdcData();
        
        CheckChargingStatus();

        // 若处于开机阶段且到达判定时间，执行一次“10次中≥5次≤阈值”的开机判定
        bool startup_check_time_reached = startup_check_pending_ && (current_time >= startup_check_after_ms_);
        if (startup_check_time_reached) {
            static uint8_t startup_window_count = 0;
            static uint8_t startup_below_count = 0;
            startup_window_count++;
            if (last_voltage_mv_ <= SHUTDOWN_CUTOFF_MV) startup_below_count++;
            if (startup_window_count >= 10) {
                if (startup_below_count >= 5) {
                    ESP_LOGW("PowerManager", "🔋 开机最终判定: 10次有%d次≤%dmV，不允许开机，执行关机", startup_below_count, SHUTDOWN_CUTOFF_MV);
                    // 清零并关机
                    startup_window_count = 0;
                    startup_below_count = 0;
                    EnterDeepSleepIfNotCharging();
                    return;
                }
                // 判定通过，启用后续关机判定
                startup_check_pending_ = false;
                shutdown_checks_enabled_ = true;
                shutdown_below_count_ = 0;
                ignore_shutdown_until_ms_ = current_time; // 允许立刻开始正常关机判定
                ESP_LOGI("PowerManager", "🔋 开机最终判定通过: ≤%dmV次数=%d/10，启用关机判定", SHUTDOWN_CUTOFF_MV, startup_below_count);
                startup_window_count = 0;
                startup_below_count = 0;
            }
            return;
        }

        // 关机保护：3.3V 放电截止（加入稳定窗口与去抖）
        uint32_t now_ms = esp_timer_get_time() / 1000;
        uint32_t v_now_mv = GetBatteryVoltage();
        last_voltage_mv_ = v_now_mv;
        // 开机阶段：在启动最终判定完成前，不允许开启关机判定
        if (!shutdown_checks_enabled_ && now_ms >= ignore_shutdown_until_ms_ && !startup_check_pending_) {
            shutdown_checks_enabled_ = true;
            shutdown_below_count_ = 0;
        }
        // 仅在通过开机最终判定后，才允许触发关机判定
        if (shutdown_checks_enabled_ && now_ms >= ignore_shutdown_until_ms_ && !startup_check_pending_) {
            if (v_now_mv <= SHUTDOWN_CUTOFF_MV) {
                if (shutdown_below_count_ < 255) shutdown_below_count_++;
            } else {
                shutdown_below_count_ = 0;
            }
            // 连续10次（约1秒）低于阈值才关机
            if (shutdown_below_count_ >= 10) {
                ESP_LOGW("PowerManager", "🔋 放电截止: 电压=%" PRIu32 "mV ≤ %dmV (连续10次), 即将关机", v_now_mv, SHUTDOWN_CUTOFF_MV);
                EnterDeepSleepIfNotCharging();
            }
        } else {
            ESP_LOGD("PowerManager", "🔋 跳过关机判定: %s 稳定窗口剩余 %d ms", 
                     startup_check_pending_ ? "开机阶段禁用," : (shutdown_checks_enabled_ ? "负载刚变化," : "禁用状态,"),
                     (int)(ignore_shutdown_until_ms_ - now_ms));
        }

        // 低电量播报（直接使用最终滤波电压与阈值比较，无回滞）
        bool prev_low = low_battery_;
        low_battery_ = (v_now_mv <= LOW_BATTERY_VOLTAGE_MV);
        if (prev_low != low_battery_) {
            ESP_LOGI("PowerManager", "🔋 低电量状态: %s (电压:%dmV, 阈值:%dmV)",
                     low_battery_ ? "进入" : "退出", (int)v_now_mv, LOW_BATTERY_VOLTAGE_MV);
            
            // 低电量状态变化时仅记录日志
            if (low_battery_) {
                // 进入低电量状态
                ESP_LOGI("PowerManager", "Enter low battery mode");
            } else {
                // 退出低电量状态
                ESP_LOGI("PowerManager", "Exit low battery mode");
            }
        }

        // 计算目标电量（纯ADC电压法）
        uint8_t soc_now = estimate_soc_from_voltage(v_now_mv);
        uint8_t calculated_battery_level = soc_now;
        
        // 更新无负载基线（无负载且稳定一段时间后才更新）
        if (!motor_running_ && !led_enabled_) {
            if (last_no_load_time_ms_ == 0) {
                last_no_load_time_ms_ = current_time;
            }
            if (current_time - last_no_load_time_ms_ >= 2000) { // 2s稳定窗口
                baseline_soc_ = calculated_battery_level;
                baseline_valid_ = true;
            }
        } else {
            last_no_load_time_ms_ = 0; // 重新计时
        }
        
        // 直接使用阶梯式电量显示，不进行平滑过渡
        battery_level_ = calculated_battery_level;
        target_battery_level_ = calculated_battery_level;
        displayed_battery_level_ = calculated_battery_level;

        // 每50次检测打印一次详细信息（约5秒一次）
        static uint32_t print_counter = 0;
        print_counter++;
        if (print_counter >= 50) {
            print_counter = 0;
            uint32_t voltage = GetBatteryVoltage();
            float actual_voltage = voltage / 1000.0f;
            auto comp_info = GetLoadCompensationInfo();
            
            ESP_LOGI("PowerManager", "🔋 ===== 电池状态详情 =====");
            ESP_LOGI("PowerManager", "🔋 实时状态: 电压=%" PRIu32 "mV, 补偿=%s%" PRIu32 "mV, 电机=%s, LED=%s, 模式=%d",
                     voltage, comp_info.is_compensating ? "+" : "无", last_compensation_mv_,
                     motor_running_ ? "开" : "关", led_enabled_ ? "开" : "关", led_mode_);
            ESP_LOGI("PowerManager", "🔋 ADC原始值: %d, 平均值: %" PRIu32 "", adc_value, average_adc);
            ESP_LOGI("PowerManager", "🔋 检测电压: %" PRIu32 "mV (%.2fV)", voltage, actual_voltage);
            ESP_LOGI("PowerManager", "🔋 电压法电量: %d%%", estimate_soc_from_voltage(voltage));
            ESP_LOGI("PowerManager", "🔋 计算电量: %d%%", target_battery_level_);
            ESP_LOGI("PowerManager", "🔋 显示电量: %d%%", displayed_battery_level_);
            ESP_LOGI("PowerManager", "🔋 系统状态: 电机=%s/%d%%, LED=%s/%d%%, 模式=%d",
                     motor_running_ ? "开" : "关", motor_speed_,
                     led_enabled_ ? "开" : "关", led_brightness_, led_mode_);
            
            auto smooth_info = GetBatterySmoothInfo();
            ESP_LOGI("PowerManager", "🔋 平滑过渡: 计算=%d%%, 显示=%d%%, 目标=%d%%, 过渡中=%s", 
                     smooth_info.calculated_level, smooth_info.displayed_level, smooth_info.target_level,
                     smooth_info.is_smoothing ? "是" : "否");
            ESP_LOGI("PowerManager", "🔋 充电状态: %s", is_charging_ ? "充电中" : "未充电");
            // 计算第一层滤波（仅三次采集）的电压和电量
            int first_layer_mv = adc_value;
            if (cali_inited_) {
                (void)adc_cali_raw_to_voltage(cali_handle_, adc_value, &first_layer_mv);
            }
            uint32_t first_layer_voltage = (uint32_t)((int64_t)first_layer_mv * VBAT_SCALE_NUM / VBAT_SCALE_DEN);
            uint8_t first_layer_soc = estimate_soc_from_voltage(first_layer_voltage);
            
            ESP_LOGI("PowerManager", "🔋 第一层滤波: ADC=%d, 电压=%" PRIu32 "mV, 电量=%d%%", adc_value, first_layer_voltage, first_layer_soc);
            ESP_LOGI("PowerManager", "🔋 最终滤波: ADC=%" PRIu32 ", 电压=%" PRIu32 "mV, 电量=%d%%", average_adc, voltage, displayed_battery_level_);
            ESP_LOGI("PowerManager", "🔋 =========================");
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }
    // 提供关机连续判定状态给外部（开机复用）
    bool IsShutdownConditionMet() const { return shutdown_below_count_ >= 10; }
    // 提供最近一次电压读数（最终滤波）
    uint32_t GetLastVoltageMv() const { return last_voltage_mv_; }
    // 启动判定是否已完成
    bool IsStartupCheckDone() const { return !startup_check_pending_; }
    // 允许/禁止关机判定（应用层在开机最终判定通过后再开启）
    static void SetShutdownChecksEnabled(bool enabled) {
        PowerManager& instance = GetInstance();
        instance.shutdown_checks_enabled_ = enabled;
        if (enabled) instance.shutdown_below_count_ = 0;
    }
    
    // 立即检测一次电量
    void CheckBatteryStatusImmediately() {
        CheckBatteryStatus();
    }

    static PowerManager& GetInstance() {
        static PowerManager instance;
        return instance;
    }

    // 设置ADC句柄（由CustomBoard调用）
    static void SetAdcHandle(adc_oneshot_unit_handle_t handle) {
        PowerManager& instance = GetInstance();
        instance.adc_handle_ = handle;
        
        // 启动定时器（如果还没有启动）
        if (instance.timer_handle_ != nullptr) {
            esp_timer_stop(instance.timer_handle_);
            ESP_ERROR_CHECK(esp_timer_start_periodic(instance.timer_handle_, 100000));  // 100ms采样
            ESP_LOGI("PowerManager", "🔋 ADC句柄已设置，定时器已启动");
            // 启动时默认禁用关机判定，待应用完成最终判定后再开启
            instance.shutdown_checks_enabled_ = false;
            // 记录起始时间，用于自动开启关机判定（延时）
            uint32_t now_ms = esp_timer_get_time() / 1000;
            // 将稳定窗口覆盖到开机最终判定结束后再延长1秒
            // 开机最终判定延后到8秒（更稳定）
            instance.startup_check_pending_ = true;
            instance.startup_check_after_ms_ = now_ms + 8000;
            instance.ignore_shutdown_until_ms_ = instance.startup_check_after_ms_ + 1000;
            instance.shutdown_below_count_ = 0;
        }
    }

    // 更新系统状态（由外部调用）
    void UpdateSystemStatus(bool motor_running, uint8_t motor_speed, bool led_enabled, uint8_t led_brightness, uint8_t led_mode = 0) {
        // 检测负载状态变化
        bool load_changed = (motor_running_ != motor_running) || (led_enabled_ != led_enabled) || (led_mode_ != led_mode);
        
        motor_running_ = motor_running;
        motor_speed_ = motor_speed;
        led_enabled_ = led_enabled;
        led_brightness_ = led_brightness;
        led_mode_ = led_mode;
        
        // 负载状态已更新
        if (load_changed) {
            ESP_LOGI("PowerManager", "🔋 负载状态变化：电机=%s, LED=%s, 模式=%d", 
                     motor_running ? "开" : "关", led_enabled ? "开" : "关", led_mode);
            // 记录负载变化时间，提供电压稳定窗口，避免滑动平均历史造成误判
            last_load_change_time_ms_ = esp_timer_get_time() / 1000; // ms
            ignore_shutdown_until_ms_ = last_load_change_time_ms_ + 1000; // 稳定1秒
            shutdown_below_count_ = 0; // 重置去抖计数
        }
    }
    
    // 获取当前负载补偿信息（用于调试和监控）
    struct LoadCompensationInfo {
        float max_current_ma;
        float total_resistance_mohm;
        float max_voltage_drop_mv;
        uint32_t fixed_compensation_mv;
        bool is_compensating;
    };
    
    LoadCompensationInfo GetLoadCompensationInfo() {
        LoadCompensationInfo info;
        info.max_current_ma = GetMaxLoadCurrent();
        info.total_resistance_mohm = BATTERY_INTERNAL_RESISTANCE_MOHM + LINE_RESISTANCE_MOHM;
        info.max_voltage_drop_mv = (info.max_current_ma * info.total_resistance_mohm) / 1000.0f;
        info.fixed_compensation_mv = (uint32_t)info.max_voltage_drop_mv + PWM_INTERFERENCE_COMPENSATION_MV;
        info.is_compensating = (motor_running_ || led_enabled_);
        return info;
    }
    
    // 获取电量平滑过渡信息
    struct BatterySmoothInfo {
        uint8_t calculated_level;    // 计算出的电量
        uint8_t displayed_level;     // 显示给用户的电量
        uint8_t target_level;        // 目标电量
        bool is_smoothing;           // 是否正在平滑过渡
    };
    
    BatterySmoothInfo GetBatterySmoothInfo() {
        BatterySmoothInfo info;
        info.calculated_level = target_battery_level_;
        info.displayed_level = displayed_battery_level_;
        info.target_level = target_battery_level_;
        info.is_smoothing = (displayed_battery_level_ != target_battery_level_);
        return info;
    }
    
    // 获取当前电池电压（公共接口）
    uint32_t GetCurrentBatteryVoltage() {
        // 检查ADC句柄是否有效
        if (adc_handle_ == nullptr) {
            ESP_LOGW("PowerManager", "🔋 ADC句柄未设置，无法读取电压");
            return 0;
        }
        
        // 如果average_adc为0，说明还没有读取过ADC数据，直接读取一次
        if (average_adc == 0) {
            int adc_value = 0;
            if (adc_oneshot_read(adc_handle_, adc_channel_, &adc_value) == ESP_OK) {
                // 临时更新average_adc用于计算
                int temp_adc = average_adc;
                average_adc = adc_value;
                uint32_t voltage = GetBatteryVoltage();
                average_adc = temp_adc; // 恢复原值
                return voltage;
            } else {
                ESP_LOGW("PowerManager", "🔋 ADC读取失败");
                return 0;
            }
        }
        return GetBatteryVoltage();
    }

    // 获取低电量状态
    bool IsLowBattery() const {
        return low_battery_;
    }

    void EnterDeepSleepIfNotCharging() {
        // 不在充电就真休眠
        if (is_charging_ == 0) {
            vb6824_shutdown();
            vTaskDelay(pdMS_TO_TICKS(200));
            // 配置唤醒源 只有电源域是VDD3P3_RTC的才能唤醒深睡
            uint64_t wakeup_pins = (BIT(GPIO_NUM_1));
            esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
            ESP_LOGI("PowerMgr", "ready to esp_deep_sleep_start");
            vTaskDelay(pdMS_TO_TICKS(10));
            
            esp_deep_sleep_start();
        }
    }
};

#endif  // __POWER_MANAGER_H__