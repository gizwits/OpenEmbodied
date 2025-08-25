#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/gpio_led.h"
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
#include "toy_data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"

#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    VbAduioCodec audio_codec;
    Button volume_up_button_;
    Button volume_down_button_;
    Button prev_button_;
    Button next_button_;


    int64_t prev_last_click_time_ = 0;
    int64_t next_last_click_time_ = 0;


    void InitializeButtons() {

        boot_button_.OnPressRepeat([this](uint16_t count) {
            if(count >= 3){
                ResetWifiConfiguration();
            } else {
                Application::GetInstance().ToggleChatState();
            }
        });

        prev_button_.OnClick([this]() {
            int64_t now = esp_timer_get_time();
            if (Application::GetInstance().GetDeviceState() == DeviceState::kDeviceStateIdle) {
                Application::GetInstance().CancelPlayMusic();
                Application::GetInstance().ToggleChatState();
                vTaskDelay(pdMS_TO_TICKS(2000));
                Application::GetInstance().SendTextToAI("给我播放一首歌");
            } else {
                if (now - prev_last_click_time_ > 10* 1000000) { // 5秒
                    prev_last_click_time_ = now;
                    Application::GetInstance().SendTextToAI("给我播放一首歌");
                }
            }
            
        });

        next_button_.OnClick([this]() {
            int64_t now = esp_timer_get_time();
            if (Application::GetInstance().GetDeviceState() == DeviceState::kDeviceStateIdle) {
                Application::GetInstance().CancelPlayMusic();
                Application::GetInstance().ToggleChatState();
                vTaskDelay(pdMS_TO_TICKS(2000));
                Application::GetInstance().SendTextToAI("给我播放一首歌");
            } else {
                if (now - prev_last_click_time_ > 10* 1000000) { // 5秒
                    prev_last_click_time_ = now;
                    Application::GetInstance().SendTextToAI("给我播放一首歌");
                }
            }
        });
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
        });

    }
    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

    void InitializeDataPointManager() {
        // 设置 ToyDataPointManager 的回调函数
        ToyDataPointManager::GetInstance().SetCallbacks(
            [this]() -> bool { return false; }, // IsCharging - toy 版本可能没有充电功能
            []() -> int { return Application::GetInstance().GetChatMode(); },
            [](int value) { Application::GetInstance().SetChatMode(value); },
            [this]() -> int { 
                return 100; // 固定电量 100%
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
            [this]() -> int { return 100; }, // 固定亮度 100%
            [this](int value) { /* toy 版本可能没有亮度调节 */ }
        );
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    prev_button_(PREV_BUTTON_GPIO), next_button_(NEXT_BUTTON_GPIO){      

        InitializeButtons();
        InitializeIot();
        InitializeDataPointManager();

        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        ESP_LOGI(TAG, "Data Point Manager initialized.");

        InitializeGpio(POWER_GPIO, true);
        InitializeGpio(POWER_GPIO, true);

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (command == "你好小智" || command.find("小云") != std::string::npos){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (command == "开始配网") {
                ResetWifiConfiguration();
            }
        });
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

    virtual Led* GetLed() override {
        static GpioLed led(LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return ToyDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return ToyDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return ToyDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return ToyDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        ToyDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        ToyDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

};

DECLARE_BOARD(CustomBoard);
