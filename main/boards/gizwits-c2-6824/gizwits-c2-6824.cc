#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
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
#include "custom/led_signal.h"
#include "power_manager.h"

#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    LedSignal* led_signal_ = nullptr;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    // PowerManager* power_manager_;
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 3, portMAX_DELAY);  // peter mark 休眠时间
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
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

        // 不在充电就真休眠
        if(PowerManager::GetInstance().IsCharging() == 0){
            vb6824_shutdown();
            vTaskDelay(pdMS_TO_TICKS(200));
            // 配置唤醒源
            uint64_t wakeup_pins = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << GPIO_NUM_1);
            esp_deep_sleep_enable_gpio_wakeup(wakeup_pins, ESP_GPIO_WAKEUP_GPIO_LOW);
            
            esp_deep_sleep_start();
        }
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
            if(count >= 3){
                ResetWifiConfiguration();
            }
        });
    }

    void InitializeLedSignal() {
        led_signal_ = new LedSignal(GPIO_NUM_2, LEDC_CHANNEL_0, GPIO_NUM_4, LEDC_CHANNEL_1, GPIO_NUM_5, LEDC_CHANNEL_2);
        // led_signal_->CycleColorsWithFade(300, 100);
        led_signal_->MonitorAndUpdateLedState();
    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
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

        InitializePowerSaveTimer();       
        InitializeButtons();
        InitializeIot();
        InitializeLedSignal();
        InitializePowerManager();


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

    uint8_t GetBatteryLevel() override {
        return PowerManager::GetInstance().GetBatteryLevel();
    }

    bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(CustomBoard);
