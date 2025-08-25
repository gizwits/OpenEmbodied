#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
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
#include "data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "driver/ledc.h"
#include "led_signal.h"
#include "power_manager.h"

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 10
#define SLEEP_TIME_SEC 60 * 3
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    // PowerManager* power_manager_;
    
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
            run_sleep_mode(true);
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

        // 检查不在充电就真休眠
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    void InitializeButtons() {

        const int chat_mode = Application::GetInstance().GetChatMode();
        rec_button_ = new Button(BUILTIN_REC_BUTTON_GPIO);

        if (chat_mode == 0) {
            rec_button_->OnPressUp([this]() {
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
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
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
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

        ESP_LOGI(TAG, "Initializing Buttons...");
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            ESP_LOGI(TAG, "Initializing LED Signal...");
            InitializeLedSignal();
        }

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();


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

        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        InitializeDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");

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

    int GetDefaultChatMode() override {
        return 0;
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

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return DataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return DataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return DataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return DataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        DataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        DataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

};

DECLARE_BOARD(CustomBoard);
