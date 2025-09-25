#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"

#include "config.h"
#include "settings.h"
#include "iot/thing_manager.h"
#include <esp_sleep.h>
#include "power_save_timer.h"
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include <wifi_station.h>

#include <esp_log.h>
#include "assets/lang_config.h"
#include "vb6824.h"
#include <esp_wifi.h>
#include "data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "power_manager.h"
#include "minimal_ws2812.h"

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 3
#define SLEEP_TIME_SEC 60 * 3
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button volume_button_;
    // Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    MinimalWS2812 led_strip_;
    bool sleep_flag_ = false;
    // PowerManager* power_manager_;

    int64_t power_on_time_ = 0;  // 记录上电时间
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, SLEEP_TIME_SEC, portMAX_DELAY);  // peter mark 休眠时间
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            run_sleep_mode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Shutting down");
        });
        power_save_timer_->OnShutdownRequest([this]() {
            
        });
        power_save_timer_->SetEnabled(true);
    }

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();

        // vb6824_shutdown();
        // vTaskDelay(pdMS_TO_TICKS(200));
        // // 配置唤醒源 只有电源域是VDD3P3_RTC的才能唤醒深睡
        // uint64_t wakeup_pins = (BIT(BOOT_BUTTON_GPIO));
        // esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
        // ESP_LOGI("PowerMgr", "ready to esp_deep_sleep_start");
        // vTaskDelay(pdMS_TO_TICKS(10));
        
        // esp_deep_sleep_start();

        gpio_set_level(POWER_HOLD_GPIO, 0);
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
       
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "Press up");
            if(sleep_flag_){
                // 直接关机
                gpio_set_level(POWER_HOLD_GPIO, 0);
            }
        });

        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
        boot_button_.OnLongPress([this]() {
            
            // 计算设备运行时间
            int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            int64_t uptime_ms = current_time - power_on_time_;
            ESP_LOGI(TAG, "设备运行时间: %lld ms", uptime_ms);
            
            // 首次上电5秒内且first_level==0才忽略
            const int64_t MIN_UPTIME_MS = 5000; // 5秒
            if (first_level == 0 && uptime_ms < MIN_UPTIME_MS) {
                first_level = 1;
                ESP_LOGI(TAG, "首次上电5秒内，忽略长按操作");
            } else {
                ESP_LOGI(TAG, "Long press");
                sleep_flag_ = true;
            }
        });

        volume_button_.OnClick([this]() {
            ESP_LOGI(TAG, "volume_button_.OnClick");
            auto codec = GetAudioCodec();
            auto current_volume = codec->output_volume();
            auto new_volume = current_volume + 10;
            
            // 循环调整音量，最小值为10
            if (new_volume > 100) {
                new_volume = 10;  // 超过100时回到最小值10
            }
            
            codec->SetOutputVolume(new_volume);
            ESP_LOGI(TAG, "Volume adjusted from %d to %d", current_volume, new_volume);
        });
        
    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        ESP_LOGI(TAG, "Initializing IoT components...");
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        ESP_LOGI(TAG, "Added IoT component: Speaker");
        thing_manager.AddThing(iot::CreateThing("Led"));
        ESP_LOGI(TAG, "Added IoT component: Led");
        ESP_LOGI(TAG, "IoT components initialization complete.");
    }

    
    void InitializePowerManager() {
        PowerManager::GetInstance();
    }

    void InitializeDataPointManager() {
        
        // 设置 DataPointManager 的回调函数
        DataPointManager::GetInstance().SetCallbacks(
            [this]() -> bool { return IsCharging(); },
            []() -> int { return Application::GetInstance().GetChatMode(); },
            [](int value) { Application::GetInstance().SetChatMode(value); },
            [this]() -> int { 
                int level = 0;
                bool charging = false, discharging = false;
                GetBatteryLevel(level, charging, discharging);
                return level;
            },
            [this]() -> int { return GetAudioCodec()->output_volume(); },
            [this](int value) { GetAudioCodec()->SetOutputVolume(value); },
            []() -> int { 
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                    return 100 - (uint8_t)abs(ap_info.rssi);
                }
                return 0;
            },
            [this]() -> int { return GetBrightness(); },
            [this](int value) { SetBrightness(value); }
        );
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO),  volume_button_(VOLUME_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO)
    {      


        power_on_time_ = esp_timer_get_time() / 1000;

        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();

        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
        }
        InitializeGpio(POWER_HOLD_GPIO, true);
        InitializeGpio(LIGHT_POWER_GPIO, true);

        ESP_LOGI(TAG, "Initializing Power Manager...");
        InitializePowerManager();
        ESP_LOGI(TAG, "Power Manager initialized.");

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                ResetWifiConfiguration();
            }
        });

        PowerManager::GetInstance().CheckBatteryStatusImmediately();
        ESP_LOGI(TAG, "Immediately check the battery level upon startup: %d", PowerManager::GetInstance().GetBatteryLevel());


        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        // InitializeDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");

        // 初始化灯带，设置默认颜色（简化版本，节省内存）
        led_strip_.SetColor(0, 255, 0);
        // strip_led_.StartBreathing(0, 255, 0);  // 绿色
        ESP_LOGI(TAG, "LED strip initialized with simplified version");

    }


    void InitializeGpio(gpio_num_t gpio_num_, bool output = false) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        if (output) {
            gpio_set_level(gpio_num_, 1);
        } else {
            gpio_set_level(gpio_num_, 0);
        }
    }

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    };


    bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    void EnterDeepSleepIfNotCharging() {
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    void SetPowerSaveTimer(bool enable) {
        power_save_timer_->SetEnabled(enable);
    }
    // 数据点相关方法实现
    // const char* GetGizwitsProtocolJson() const override {
    //     return DataPointManager::GetInstance().GetGizwitsProtocolJson();
    // }

    // size_t GetDataPointCount() const override {
    //     return DataPointManager::GetInstance().GetDataPointCount();
    // }

    // bool GetDataPointValue(const std::string& name, int& value) const override {
    //     return DataPointManager::GetInstance().GetDataPointValue(name, value);
    // }

    // bool SetDataPointValue(const std::string& name, int value) override {
    //     return DataPointManager::GetInstance().SetDataPointValue(name, value);
    // }

    // void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
    //     DataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    // }

    // void ProcessDataPointValue(const std::string& name, int value) override {
    //     DataPointManager::GetInstance().ProcessDataPointValue(name, value);
    // }

};

DECLARE_BOARD(CustomBoard);
