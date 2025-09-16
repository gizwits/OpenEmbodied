#ifndef __POWER_MANAGER_H__
#define __POWER_MANAGER_H__

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <functional>

class PowerManager {
private:
    // 电池电量区间-分压电阻为2个100k
    static constexpr struct {
        uint16_t adc;
        uint8_t level;
    } BATTERY_LEVELS[] = {{1750, 0}, {2010, 100}};
    static constexpr size_t BATTERY_LEVELS_COUNT = 2;
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

    static constexpr uint8_t MAX_CHANGE_COUNT = 3;
    static constexpr uint32_t TIME_LIMIT = 2000000; // 2 seconds in microseconds

    uint8_t change_count_ = 0;  // 记录状态变化次数
    uint64_t last_change_time_ = 0;  // 最后一次状态变化的时间戳（微秒）

    adc_oneshot_unit_handle_t adc_handle_;

    // 充电状态改变回调函数
    std::function<void(bool)> charging_status_callback_ = nullptr;

    void CheckBatteryStatus() {
        uint64_t current_time = esp_timer_get_time(); // 获取当前时间（微秒）
        // 先读取一次并更新平均值、电量，再基于平均值判定充电状态
        uint32_t average_adc = ReadBatteryAdcData();

        // 基于电压阈值判断是否在充电
        // 分压为 1:1 → ADC 端阈值电压为 2.8V。
        static constexpr uint32_t ADC_RAW_THRESHOLD_CHARGING = 2400;

        bool new_is_charging = average_adc >= ADC_RAW_THRESHOLD_CHARGING;

        // ESP_LOGI("PowerManager", "new_is_charging: %d, is_charging_: %d, average_adc: %u", new_is_charging, is_charging_, (unsigned)average_adc);
        // 如果状态有变化
        if (new_is_charging != is_charging_) {
            bool old_charging_status = is_charging_;
            is_charging_ = new_is_charging;
            change_count_++;  // 增加变化次数
            last_change_time_ = current_time;  // 更新最后变化时间
            
            // 调用充电状态改变回调
            if (charging_status_callback_) {
                charging_status_callback_(is_charging_);
            }
        }
    }
    uint32_t ReadBatteryAdcData() {
        int adc_value;
        esp_err_t err = adc_oneshot_read(adc_handle_, adc_channel_, &adc_value);
        ESP_ERROR_CHECK(err);

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

        CalculateBatteryLevel(average_adc);


        // ESP_LOGI("PowerManager", "ADC值: %d 平均值: %ld 电量: %u%%", adc_value, average_adc,
        //          battery_level_);
        return average_adc;
    }

    void CalculateBatteryLevel(uint32_t average_adc) {
        if (average_adc <= BATTERY_LEVELS[0].adc) {
            battery_level_ = 0;
        } else if (average_adc >= BATTERY_LEVELS[BATTERY_LEVELS_COUNT - 1].adc) {
            battery_level_ = 100;
        } else {
            float ratio = static_cast<float>(average_adc - BATTERY_LEVELS[0].adc) /
                          (BATTERY_LEVELS[1].adc - BATTERY_LEVELS[0].adc);
            battery_level_ = ratio * 100;
        }
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

    // 设置充电状态改变回调函数
    void SetChargingStatusCallback(std::function<void(bool)> callback) {
        charging_status_callback_ = callback;
    }
    void EnterDeepSleepIfNotCharging() {
        ESP_LOGI("PowerManager", "EnterDeepSleepIfNotCharging");
        // 不在充电就真休眠
        if (is_charging_ == 0) {
            // 非充电 直接关机
            gpio_set_level(POWER_HOLD_GPIO, 0);
        } else {
            // 充电中，进休眠
            // vb6824_shutdown();
            // vTaskDelay(pdMS_TO_TICKS(200));
            // // 配置唤醒源 只有电源域是VDD3P3_RTC的才能唤醒深睡
            // uint64_t wakeup_pins = (BIT(POWER_BUTTON_GPIO));
            // esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
            // ESP_LOGI("PowerMgr", "ready to esp_deep_sleep_start");
            // vTaskDelay(pdMS_TO_TICKS(10));
            
            // esp_deep_sleep_start();
        }
    }
};
#endif  // __POWER_MANAGER_H__
