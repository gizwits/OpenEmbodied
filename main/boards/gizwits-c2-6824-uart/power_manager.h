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

    static constexpr uint8_t MAX_CHANGE_COUNT = 8;
    static constexpr uint32_t TIME_LIMIT = 2000000; // 2 seconds in microseconds

    uint8_t change_count_ = 0;  // 记录状态变化次数
    uint64_t last_change_time_ = 0;  // 最后一次状态变化的时间戳（微秒）

    adc_oneshot_unit_handle_t adc_handle_;

    // 电压-电量对照表
    static constexpr struct VoltageSocPair {
        uint16_t adcValue; // ADC value
        uint8_t soc;       // State of Charge (percentage of battery capacity)
    } dischargeCurve[] = {
        {4200, 100}, // 满电压附近
        {4160, 95},
        {4120, 90},
        {4080, 85},
        {4040, 80},
        {4000, 75},
        {3960, 70},
        {3920, 60},
        {3880, 50},
        {3850, 40},
        {3820, 35},
        {3790, 30},
        {3760, 25},
        {3720, 20},
        {3680, 15},
        {3600, 10},
        {3500, 5},
        {3400, 0},
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

        ReadBatteryAdcData();

        #define BATTERY_FULL_VOLTAGE 4300
        #define BATTERY_NOT_CHARGING_VOLTAGE 4200

        static uint8_t not_charging_count = 0;
        uint32_t voltage = average_adc == 0 ? adc_value*2 : average_adc*2;
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

        CalculateBatteryLevel(average_adc*2);

        // if(times++ % 50 == 0){
        //     ESP_LOGI("PowerManager", "adc: %d adc_avg: %ld, VBAT: %ld, battery_level_: %u%%", 
        //         adc_value, average_adc, average_adc*2, battery_level_);
        // }
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        battery_level_ = estimate_soc(average_adc);
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
    
    // 立即检测一次电量
    void CheckBatteryStatusImmediately() {
        CheckBatteryStatus();
    }

    static PowerManager& GetInstance() {
        static PowerManager instance; // 使用默认构造函数初始化对象
        return instance;
    }

    
};
#endif  // __POWER_MANAGER_H__
