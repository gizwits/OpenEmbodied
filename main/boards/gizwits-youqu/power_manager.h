#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>

class PowerManager {
private:
    // 电池电量区间-分压电阻为2个100k
    static constexpr struct {
        uint16_t adc;
        uint8_t level;
    } BATTERY_LEVELS[] = {
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
        {3420, 10},  // 下降150mV（低电量段开始）
        {3220, 5},   // 下降200mV
        {3000, 0}    // 下降220mV
    };
    static constexpr size_t BATTERY_LEVELS_COUNT = sizeof(BATTERY_LEVELS) / sizeof(BATTERY_LEVELS[0]);
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

    adc_oneshot_unit_handle_t adc_handle_;

    void CheckBatteryStatus() {
        uint64_t current_time = esp_timer_get_time(); // 获取当前时间（微秒）

        // 如果时间间隔超过2秒，则重置状态变化计数
        if (current_time - last_change_time_ > TIME_LIMIT) {
            change_count_ = 0;
        }

        if (change_count_ < MAX_CHANGE_COUNT) {
            bool new_is_charging = gpio_get_level(bat_led_pin_) != 0;  // 检查LED引脚状态

            // 判断充电引脚状态
            if (new_is_charging) {
                new_is_charging = gpio_get_level(charging_pin_) == 1;
            }

            // 如果状态有变化
            if (new_is_charging != is_charging_) {
                is_charging_ = new_is_charging;
                change_count_++;  // 增加变化次数
                last_change_time_ = current_time;  // 更新最后变化时间
            }
        }

        ReadBatteryAdcData();
    }
    void ReadBatteryAdcData() {
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

        // ADC 采样是 1/2 分压，需要乘以 2 得到实际电压值（mV）
        uint32_t actual_voltage_mv = average_adc * 2;
        CalculateBatteryLevel(actual_voltage_mv);


        // ESP_LOGI("PowerManager", "ADC值: %d 平均值: %ld 电量: %u%%", adc_value, average_adc,
        //          battery_level_);
    }

    void CalculateBatteryLevel(uint32_t voltage_mv) {
        // BATTERY_LEVELS 数组按电压从高到低排列（100% 到 0%）
        // 查找电压对应的电量等级
        if (voltage_mv >= BATTERY_LEVELS[0].adc) {
            // 电压高于或等于最高阈值，电量为 100%
            battery_level_ = BATTERY_LEVELS[0].level;
        } else if (voltage_mv <= BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].adc) {
            // 电压低于或等于最低阈值，电量为 0%
            battery_level_ = BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].level;
        } else {
            // 在中间范围，进行线性插值
            // 数组从高到低排列，所以需要找到 voltage_mv 落在哪个区间
            for (size_t i = 0; i < BATTERY_LEVELS_COUNT - 1; i++) {
                // 因为数组从高到低，所以 BATTERY_LEVELS[i].adc > BATTERY_LEVELS[i+1].adc
                if (voltage_mv <= BATTERY_LEVELS[i].adc && voltage_mv > BATTERY_LEVELS[i + 1].adc) {
                    // 在两个相邻点之间进行线性插值
                    float ratio = static_cast<float>(voltage_mv - BATTERY_LEVELS[i + 1].adc) /
                                  static_cast<float>(BATTERY_LEVELS[i].adc - BATTERY_LEVELS[i + 1].adc);
                    battery_level_ = static_cast<uint8_t>(
                        BATTERY_LEVELS[i + 1].level + 
                        ratio * (BATTERY_LEVELS[i].level - BATTERY_LEVELS[i + 1].level)
                    );
                    return;
                }
            }
            // 如果没找到匹配区间，使用最后一个值（0%）
            battery_level_ = BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].level;
        }
    }

public:
    PowerManager(gpio_num_t charging_pin, gpio_num_t bat_led_pin, adc_unit_t adc_unit = ADC_UNIT_2,
                 adc_channel_t adc_channel = ADC_CHANNEL_3)
        : charging_pin_(charging_pin), bat_led_pin_(bat_led_pin), adc_unit_(adc_unit), adc_channel_(adc_channel) {

        // 配置充电引脚
        // gpio_config_t io_conf = {};
        // io_conf.intr_type = GPIO_INTR_DISABLE;
        // io_conf.mode = GPIO_MODE_INPUT;
        // io_conf.pin_bit_mask = (1ULL << charging_pin_);
        // io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        // io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        // gpio_config(&io_conf);

        // // 配置状态引脚
        // io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        // io_conf.pin_bit_mask = (1ULL << bat_led_pin_);
        // gpio_config(&io_conf);

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
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 500000));  // 5秒

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
};
#endif  // __POWER_MANAGER_H__
