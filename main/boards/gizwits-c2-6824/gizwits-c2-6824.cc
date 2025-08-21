#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "settings.h"
#include "led/gpio_led.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include <esp_sleep.h>
#include "power_save_timer.h"
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include <wifi_station.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "vb6824.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "driver/ledc.h"
#include "led_signal.h"
#include "power_manager.h"

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 3
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
                ESP_LOGI(TAG, "rec_button_.OnPressUp");
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                auto &app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StartListening();
            });
        } else {
            rec_button_->OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
            boot_button_.OnClick([this]() {
                ESP_LOGI(TAG, "boot_button_.OnClick");
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

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();

        ESP_LOGI(TAG, "Initializing LED Signal...");
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            InitializeLedSignal();
        }

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

    }

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    };

    uint8_t GetBatteryLevel() override {
        return PowerManager::GetInstance().GetBatteryLevel();
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

    uint8_t GetBrightness() {
        return LedSignal::GetInstance().GetBrightness();
    }
    
    void SetBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
    }

    uint8_t GetDefaultBrightness() {
        return LedSignal::GetInstance().GetDefaultBrightness();
    }

};

DECLARE_BOARD(CustomBoard);
