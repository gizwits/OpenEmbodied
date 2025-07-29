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
#include <esp_timer.h>
#include "power_manager.h"
#include "assets/lang_config.h"


#define TAG "GizwitsDev"

class GizwitsDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    adc_oneshot_unit_handle_t adc1_handle_;  // ADC句柄

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button rec_button_;
    PowerManager* power_manager_;
    
    bool need_power_off_ = false;
    
    // 按钮事件队列
    QueueHandle_t button_event_queue_;

    int64_t rec_last_click_time_ = 0;
    int64_t power_on_time_ = 0;  // 记录上电时间
    
    // 双按钮长按检测相关变量
    bool volume_up_long_pressed_ = false;
    bool volume_down_long_pressed_ = false;
    int64_t dual_long_press_time_ = 0;

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
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        // boot_button_.OnPressRepeat([this](uint16_t count) {
        //     ESP_LOGI(TAG, "boot_button_.OnPressRepeat");
        //     if(count >= 3){
        //         ResetWifiConfiguration();
        //     } else {
        //         // Application::GetInstance().ToggleChatState();
        //     }
        // });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateIdle);
            // 低电平代表是按照开机键上电的，可以忽略
            
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
              
                ESP_LOGI(TAG, "执行关机操作");
                Application::GetInstance().QuitTalking();
                auto codec = GetAudioCodec();
                codec->EnableOutput(true);
                Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<GizwitsDevBoard*>(arg);
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);


                    if (board->isCharging()) {
                        // 充电中
                        // 关灯
                        Application::GetInstance().SetDeviceState(kDeviceStatePowerOff);

                    } else {
                        gpio_set_level(POWER_HOLD_GPIO, 0);
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        auto chat_mode = Application::GetInstance().GetChatMode();
        ESP_LOGI(TAG, "chat_mode: %d", chat_mode);

        if (chat_mode == 0) {
            rec_button_.OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                Application::GetInstance().StartListening();
            });
            rec_button_.OnPressUp([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressUp");
                Application::GetInstance().StopListening();
            });
        } else {
            rec_button_.OnClick([this]() {
                ESP_LOGI(TAG, "rec_button_.OnClick");
                Application::GetInstance().ToggleChatState();
            });
        }
        
        volume_up_button_.OnClick([this]() {
            ESP_LOGI(TAG, "volume_up_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            codec->SetOutputVolume(volume);
        });
        volume_up_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "volume_up_button_.OnLongPress");
            CheckDualLongPress();
        });

        volume_down_button_.OnClick([this]() {
            ESP_LOGI(TAG, "volume_down_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            codec->SetOutputVolume(volume);
        });
        volume_down_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "volume_down_button_.OnLongPress");
            CheckDualLongPress();
        });

    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }


    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(GPIO_NUM_NC, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);
    }


public:
    GizwitsDevBoard() : boot_button_(BOOT_BUTTON_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    rec_button_(REC_BUTTON_GPIO) {
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);
        
        InitializeButtons();
        InitializeGpio(POWER_HOLD_GPIO, true);
        InitializeChargingGpio();
        InitializeI2c();
        InitializeIot();
        InitializePowerManager();
    }


    ~GizwitsDevBoard() {
        // 清理ADC资源
        if (adc1_handle_) {
            adc_oneshot_del_unit(adc1_handle_);
        }
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


    void InitializeChargingGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << CHARGING_PIN) | (1ULL << STANDBY_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }

    bool isCharging() {
        int chrg = gpio_get_level(CHARGING_PIN);
        int standby = gpio_get_level(STANDBY_PIN);
        ESP_LOGI(TAG, "chrg: %d, standby: %d", chrg, standby);
        return chrg == 0 || standby == 0;
    }
    
    void CheckDualLongPress() {
        int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
        
        // 检查当前哪个按钮被长按
        if (gpio_get_level(VOLUME_UP_BUTTON_GPIO) == 0) {
            volume_up_long_pressed_ = true;
        }
        if (gpio_get_level(VOLUME_DOWN_BUTTON_GPIO) == 0) {
            volume_down_long_pressed_ = true;
        }
        
        // 如果两个按钮都被长按
        if (volume_up_long_pressed_ && volume_down_long_pressed_) {
            if (dual_long_press_time_ == 0) {
                dual_long_press_time_ = current_time;
                ESP_LOGI(TAG, "开始检测双按钮长按");
            } else {
                // 检查是否已经长按足够时间（比如2秒）
                const int64_t DUAL_LONG_PRESS_DURATION = 2000; // 2秒
                if (current_time - dual_long_press_time_ >= DUAL_LONG_PRESS_DURATION) {
                    ESP_LOGI(TAG, "双按钮长按触发 - ResetWifiConfiguration");
                    ResetWifiConfiguration();
                    // 重置状态
                    volume_up_long_pressed_ = false;
                    volume_down_long_pressed_ = false;
                    dual_long_press_time_ = 0;
                }
            }
        } else {
            // 如果任一按钮释放，重置状态
            if (!volume_up_long_pressed_ || !volume_down_long_pressed_) {
                volume_up_long_pressed_ = false;
                volume_down_long_pressed_ = false;
                dual_long_press_time_ = 0;
            }
        }
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = isCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
        return true;
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
