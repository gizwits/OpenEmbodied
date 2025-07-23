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

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    prev_button_(PREV_BUTTON_GPIO), next_button_(NEXT_BUTTON_GPIO){      

        InitializeButtons();
        InitializeIot();

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

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(CustomBoard);
