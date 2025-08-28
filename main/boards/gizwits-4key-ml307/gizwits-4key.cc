#include "dual_network_board.h"
#include "audio/codecs/es8311_audio_codec.h"
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

class GizwitsDevBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    adc_oneshot_unit_handle_t adc1_handle_;  // ADC句柄

    Button boot_button_;
    Button rec_button_;
    // PowerManager* power_manager_;
    
    bool need_power_off_ = false;
    
    // 按钮事件队列
    QueueHandle_t button_event_queue_;

    int64_t rec_last_click_time_ = 0;
    int64_t power_on_time_ = 0;  // 记录上电时间
    
    // 双按钮长按检测相关变量
    bool volume_up_long_pressed_ = false;
    bool volume_down_long_pressed_ = false;
    int64_t dual_long_press_time_ = 0;
    bool is_sleep_ = false;
    // CircularStrip* led_strip_;

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
        boot_button_.OnClick([this]() {
            WakeUp();
        }); 
        // boot_button_.OnPressRepeat([this](uint16_t count) {
        //     ESP_LOGI(TAG, "boot_button_.OnPressRepeat");
        //     if(count >= 5){
        //         RunResetWifiConfiguration();
        //     } else {
        //         Application::GetInstance().ToggleChatState();
        //     }
        // });


        // void OnPressRepeaDone(std::function<void(uint16_t)> callback);
        boot_button_.OnPressRepeaDone([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeaDone, count: %d", count);
            if(count == 5){
                SwitchNetworkType();
                return;
            }
            if(count >= 3){
                RunResetWifiConfiguration();
            }
        });


        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
            auto& app = Application::GetInstance();
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
                // 提前播放音频
                // 非休眠模式才播报
                if (!is_sleep_) {
                    ESP_LOGI(TAG, "执行关机操作");
                    Application::GetInstance().QuitTalking();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    auto codec = GetAudioCodec();
                    gpio_set_level(BUILTIN_SINGLE_LED_GPIO, 1);
                    codec->EnableOutput(true);
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                    need_power_off_ = true;
                }
                
            }
        });
        boot_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<GizwitsDevBoard*>(arg);
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);


                    if (board->isCharging()) {
                        // 关灯
                        board->GetLed()->TurnOff();
                        board->is_sleep_ = true;
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
                WakeUp();
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                Application::GetInstance().StartListening();
            });
            rec_button_.OnPressUp([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressUp");
                Application::GetInstance().StopListening();
            });
        } else {
            rec_button_.OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                WakeUp();
                Application::GetInstance().ToggleChatState();
            });
        }
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

    // void InitializeLedStrip() {
    //     led_strip_ = new CircularStrip(BUILTIN_LED_GPIO, 4);
    // }


    // void InitializePowerManager() {
    //     power_manager_ =
    //         new PowerManager(GPIO_NUM_NC, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);
    // }

    void WakeUp() {
        is_sleep_ = false;
        gpio_set_level(BUILTIN_SINGLE_LED_GPIO, 0);
    }


public:
    GizwitsDevBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN), boot_button_(BOOT_BUTTON_GPIO),
    rec_button_(REC_BUTTON_GPIO) {
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);
        
        InitializeButtons();
        InitializeGpio(POWER_HOLD_GPIO, true);
        InitializeGpio(BUILTIN_SINGLE_LED_GPIO, false);
        InitializeChargingGpio();
        InitializeI2c();
        InitializeIot();

        // InitializeLedStrip();
        // InitializePowerManager();
        
        // if (power_manager_) {
        //     power_manager_->CheckBatteryStatusImmediately();
        //     ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
        // }
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
    

    virtual void RunResetWifiConfiguration() {
        if (GetNetworkType() == NetworkType::WIFI) {
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.ResetWifiConfiguration();
        }
    }

    // virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
    //     charging = isCharging();
    //     discharging = !charging;
    //     level = power_manager_->GetBatteryLevel();
    //     ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
    //     return true;
    // }

    // virtual Led* GetLed() override {
    //     return led_strip_;
    // }
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
