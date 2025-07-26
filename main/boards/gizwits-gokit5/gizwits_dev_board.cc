#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include "led/circular_strip.h"

#define TAG "GizwitsDev"


class GizwitsDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button prev_button_;
    Button next_button_;

    int64_t prev_last_click_time_ = 0;
    int64_t next_last_click_time_ = 0;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
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
    GizwitsDevBoard() : boot_button_(BOOT_BUTTON_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    prev_button_(PREV_BUTTON_GPIO), next_button_(NEXT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
        InitializeIot();
    }

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 4);
        return &led;
    }
    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_1, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA, 
            AUDIO_CODEC_ES8311_ADDR, 
            false);

        return &audio_codec;
    }

};

DECLARE_BOARD(GizwitsDevBoard);
