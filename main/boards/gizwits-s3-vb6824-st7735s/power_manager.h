#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <functional>

// VBAT 缩放系数（默认1:1分压→×2）。如硬件分压不同，可在外部覆盖
#ifndef VBAT_SCALE_NUM
#define VBAT_SCALE_NUM 2
#endif
#ifndef VBAT_SCALE_DEN
#define VBAT_SCALE_DEN 1
#endif

class PowerManager {
private:
    // 放电曲线（单位: mV，使用 1:1 分压，故 VBAT ≈ 原始ADC电压*2 的估算）
    static constexpr struct VoltageSocPair {
        uint16_t mv;
        uint8_t soc;
    } DISCHARGE_CURVE[] = {
        {4140, 100}, {4104, 95}, {4068, 90}, {4032, 85}, {3996, 80},
        {3960, 75}, {3924, 70}, {3888, 65}, {3852, 60}, {3829, 55},
        {3808, 50}, {3787, 45}, {3766, 40}, {3745, 35}, {3724, 30},
        {3703, 25}, {3672, 20}, {3570, 15}, {3420, 10}, {3220, 5},
        {3000, 0}
    };
    static constexpr size_t DISCHARGE_CURVE_COUNT = sizeof(DISCHARGE_CURVE) / sizeof(DISCHARGE_CURVE[0]);
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

    // 去除旧的充电状态抖动/滞回统计变量

    adc_oneshot_unit_handle_t adc_handle_;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    bool adc_calibrated_ = false;

    // 最近一次估算电池电压(mV)
    uint32_t last_vbat_mv_ = 0;

    // 充电状态改变回调函数
    std::function<void(bool)> charging_status_callback_ = nullptr;

    void CheckBatteryStatus() {
        // 更新ADC平均与电量
        uint32_t average_adc = ReadBatteryAdcData();

        // 充电状态判定：无滞回/无保持
        static constexpr uint32_t BATTERY_CHARGING_THRESHOLD_MV = 4400;
        bool new_is_charging = (last_vbat_mv_ >= BATTERY_CHARGING_THRESHOLD_MV);
        if (new_is_charging != is_charging_) {
            is_charging_ = new_is_charging;
            if (charging_status_callback_) charging_status_callback_(is_charging_);
        }
    }
    uint32_t ReadBatteryAdcData() {
        int adc_value;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, adc_channel_, &adc_value));

        adc_values_[adc_values_index_] = adc_value;
        adc_values_index_ = (adc_values_index_ + 1) % ADC_VALUES_COUNT;
        if (adc_values_count_ < ADC_VALUES_COUNT) {
            adc_values_count_++;
        }

        uint32_t average_adc = 0;
        for (size_t i = 0; i < adc_values_count_; i++) {
            average_adc += adc_values_[i];
        }
        average_adc /= adc_values_count_;

        // 估算电压并计算电量
        // 将平均原始ADC值转换为mV（使用eFuse校准），再换算VBAT
        int voltage_mv = 0;
        if (adc_calibrated_) {
            if (adc_cali_raw_to_voltage(adc_cali_handle_, (int)average_adc, &voltage_mv) != ESP_OK) {
                voltage_mv = 0;
            }
        } else {
            // 无校准可用，使用经验近似：按12bit和11dB衰减估算，Vref约1100mV → 粗略比例
            // 这个分支仅作为兜底，精度有限
            voltage_mv = (int)((average_adc * 1100UL) / 4095UL);
        }

        uint32_t vbat_mv = (uint32_t)((int64_t)voltage_mv * VBAT_SCALE_NUM / VBAT_SCALE_DEN);
        last_vbat_mv_ = vbat_mv;
        CalculateBatteryLevel(vbat_mv);

        static uint8_t log_counter = 0;
        if ((log_counter++ % 50) == 0) {
            ESP_LOGI("PowerManager", "电池电压(估算): %lu mV | ADC原始: %d | 平均: %lu | 电量: %u%%",
                     (unsigned long)vbat_mv, adc_value, (unsigned long)average_adc, battery_level_);
        }
        return average_adc;
    }

    void CalculateBatteryLevel(uint32_t vbat_mv) {
        // 最近邻查表
        uint16_t closest_mv = DISCHARGE_CURVE[0].mv;
        uint8_t closest_soc = DISCHARGE_CURVE[0].soc;
        uint32_t min_diff = (vbat_mv > closest_mv) ? (vbat_mv - closest_mv) : (closest_mv - vbat_mv);
        for (size_t i = 1; i < DISCHARGE_CURVE_COUNT; i++) {
            uint16_t mv = DISCHARGE_CURVE[i].mv;
            uint32_t diff = (vbat_mv > mv) ? (vbat_mv - mv) : (mv - vbat_mv);
            if (diff < min_diff) {
                min_diff = diff;
                closest_mv = mv;
                closest_soc = DISCHARGE_CURVE[i].soc;
            }
        }
        battery_level_ = closest_soc;
    }

public:
    PowerManager(gpio_num_t charging_pin, gpio_num_t bat_led_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3)
        : charging_pin_(charging_pin), bat_led_pin_(bat_led_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {

        gpio_config_t io_conf = {};

        if (charging_pin_ != GPIO_NUM_NC) {
            // 配置充电引脚
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pin_bit_mask = (1ULL << charging_pin_);
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&io_conf);
        }
        

        if (bat_led_pin_ != GPIO_NUM_NC) {
            // 配置状态引脚
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pin_bit_mask = (1ULL << bat_led_pin_);
            gpio_config(&io_conf);
        }

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
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 100000));  // 5秒

        // 初始化ADC
        InitializeAdc();
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

        // 创建校准句柄（使用 curve fitting 方案，参考指定项目兼容性）
        adc_cali_curve_fitting_config_t cali_cfg_cf = {
            .unit_id = adc_unit_,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg_cf, &adc_cali_handle_) == ESP_OK) {
            adc_calibrated_ = true;
            ESP_LOGI("PowerManager", "ADC 标定已启用（curve fitting）");
        } else {
            adc_cali_handle_ = nullptr;
            adc_calibrated_ = false;
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
        if (adc_calibrated_ && adc_cali_handle_) {
            adc_cali_delete_scheme_curve_fitting(adc_cali_handle_);
        }
    }

    bool IsCharging() { return is_charging_; }

    uint8_t GetBatteryLevel() { return battery_level_; }
    
    // 立即检测一次电量
    void CheckBatteryStatusImmediately() {
        CheckBatteryStatus();
    }

    // 设置充电状态改变回调函数
    void SetChargingStatusCallback(std::function<void(bool)> callback) {
        charging_status_callback_ = callback;
    }
    void EnterDeepSleepIfNotCharging() {
        ESP_LOGI("PowerManager", "EnterDeepSleepIfNotCharging");
        if (!is_charging_) {
            // 非充电：直接拉低保持脚，真正关机
            gpio_set_level(POWER_HOLD_GPIO, 0);
        } else {
            // 充电：保持上电（不关机不深睡），与 C2 行为一致，由上层决定是否重启进入静默
            ESP_LOGI("PowerManager", "充电中，保持上电");
        }
    }
};
#endif  // __POWER_MANAGER_H__
