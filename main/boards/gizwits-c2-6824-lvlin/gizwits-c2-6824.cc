#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "led/gpio_led.h"
#include "led/single_led.h"
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
#include "lvlin_data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "driver/ledc.h"
#include "led_signal.h"
#include "power_manager.h"
#include <esp_timer.h>
#include <inttypes.h>

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 5
#define SLEEP_TIME_SEC 60 * 3
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    // Button collision_button;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    uint32_t power_on_time_;  // 上电时间戳
    // 碰撞连续触发检测
    int64_t collision_last_ts_us_ = 0;
    int64_t collision_accum_us_ = 0;
    static constexpr int64_t COLLISION_MAX_INTERVAL_US = 300000;   // 300ms
    static constexpr int64_t COLLISION_THRESHOLD_US    = 600000;  // 600ms
    static constexpr int64_t COLLISION_WAKE_THRESHOLD_US    = 1000000;  // 600ms
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人", "你好冬冬"};
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
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();

        // 检查不在充电就真休眠
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    void InitializeButtons() {

        const int chat_mode = Application::GetInstance().GetChatMode();
        rec_button_ = new Button(BUILTIN_REC_BUTTON_GPIO);

        if (chat_mode == 0) {
            rec_button_->OnPressUp([this]() {
                // 检查是否已经过了5秒
                if ((esp_timer_get_time() / 1000 - power_on_time_) < 5000) {
                    return;
                }
                
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
                // 检查是否已经过了5秒
                if ((esp_timer_get_time() / 1000 - power_on_time_) < 5000) {
                    return;
                }
                
                auto &app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StartListening();
            });
        } else {
            rec_button_->OnPressDown([this]() {
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
            boot_button_.OnClick([this]() {
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
        }

        boot_button_.OnLongPress([this]() {
            run_sleep_mode(true);
        });
        rec_button_->OnLongPress([this]() {
            run_sleep_mode(true);
        });

        // collision_button.OnPressDown([this]() {
        //     ESP_LOGI(TAG, "collision_button.OnClick");
        //     // 连续触发 1.5s，间隔<=300ms 视为有效
        //     int64_t now = esp_timer_get_time();
        //     if (collision_last_ts_us_ != 0 && (now - collision_last_ts_us_) <= COLLISION_MAX_INTERVAL_US) {
        //         collision_accum_us_ += (now - collision_last_ts_us_);
        //     } else {
        //         // 超时或首次触发，重置累计
        //         collision_accum_us_ = 0;
        //     }
        //     collision_last_ts_us_ = now;

        //     if (collision_accum_us_ >= COLLISION_THRESHOLD_US) {
        //         collision_accum_us_ = 0;
        //         collision_last_ts_us_ = 0;
        //         auto &app = Application::GetInstance();
        //         app.ToggleChatState();
        //     }
        // });

        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
        rec_button_->OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "rec_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
    }

    void InitializeLedSignal() {
        LedSignal::GetInstance().MonitorAndUpdateLedState_timer();
    }

    void SetLedBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
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
        // 设置 LvlinDataPointManager 的回调函数
        LvlinDataPointManager::GetInstance().SetCallbacks(
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
            [this](int value) { SetBrightness(value); },
            [this]() -> int { return GetSpeed_(); },
            [this](int value) { SetSpeed(value); }
        );
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
        power_on_time_ = esp_timer_get_time() / 1000;  // 记录上电时间（毫秒）

        InitializePowerManager();
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        // 如果是从深度睡眠被碰撞 GPIO 唤醒，则先等待稳定摇晃，否则重新睡眠
        // WaitForCollisionShakeOrSleepIfWokenByCollision();

        if (s_factory_test_mode == 0) {
            InitializeLedSignal();
            InitializeButtons();
        }

        
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(BUILTIN_LED_GPIO, 0);

        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();



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

        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        InitializeDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");

    }

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    };

    virtual bool NeedSilentStartup() override {
        return false;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    virtual bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    int GetDefaultChatMode() override {
        return 1;
    }

    virtual bool NeedPlayProcessVoiceWithLife() override {
        // 自然对话也要播放提示音
        return true;
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

    uint8_t GetBrightness() {
        return LedSignal::GetInstance().GetBrightness();
    }
    
    void SetBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
    }

    uint8_t GetDefaultBrightness() {
        return LedSignal::GetInstance().GetDefaultBrightness();
    }

    // 语速相关方法
    int GetSpeed_() {
        // 从设置中获取语速，默认值为0（对应正常语速）
        Settings settings("wifi", true);
        return settings.GetInt("speed", 50);
    }
    int GetVoiceSpeed() {
        int speed = GetSpeed_();
        return speed - 50;
    }
    void SetSpeed(int speed) {
        // 限制语速值在有效范围内 (0-200, 对应-50%到150%)
        int clamped_speed = std::max(0, std::min(200, speed));
        Settings settings("wifi", true);
        settings.SetInt("speed", clamped_speed);
        ESP_LOGI(TAG, "Speed set to: %d", clamped_speed);

        MqttClient::getInstance().GetRoomInfo(false);
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return LvlinDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return LvlinDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return LvlinDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return LvlinDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        LvlinDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        LvlinDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

};

DECLARE_BOARD(CustomBoard);
