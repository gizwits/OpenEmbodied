#include "dual_network_board.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "audio/codecs/vb6824_audio_codec.h"

#include "iot/thing_manager.h"
#include "power_manager.h"
#include "assets/lang_config.h"
#include "font_awesome_symbols.h"
#include "wifi_connection_manager.h"


#include "led/single_led.h"
#include "display/lcd_display.h"
#include "display/display.h"

#include <wifi_station.h>
#include "power_save_timer.h"
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"

#include <math.h>

#define TAG "MovecallMojiESP32S3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class MovecallMojiESP32S3 : public DualNetworkBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button reset_button_;
    Button break_button_;
    SpiLcdDisplay* display_;

    bool need_power_off_ = false;
    VbAduioCodec audio_codec;

    i2c_master_bus_handle_t i2c_bus_;
    int64_t power_on_time_ = 0;  // 记录上电时间
    PowerManager* power_manager_;
    PowerSaveTimer* power_save_timer_;
    bool is_charging_sleep_ = false;

    // std::vector<TestItem> test_items = {
    //     {"lcd", "LCD测试", 1},
    //     {"key", "按键测试", 0},
    //     {"wifi", "WiFi连接测试", 0},
    //     {"sensor", "陀螺仪测试", 0},
    //     {"battery", "电池检测", 0},
    //     {"mic", "麦克风检测", 0},
    // };

    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};




    void InitializePowerSaveTimer() {
        // 20 分钟进休眠
        // 30 分钟 关机
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 20, 60 * 30);
        // power_save_timer_ = new PowerSaveTimer(-1, 20 * 1, 60 * 2);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGE(TAG, "Enabling sleep mode");
            if(IsCharging()) {
                // 充电中
                is_charging_sleep_ = true;
                Application::GetInstance().Schedule([this]() {
                    Application::GetInstance().QuitTalking();
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);

                    // 在这个场景里要切换成睡觉表情 
                    // display_->SetEmotion("sleepy");
                }, "EnterSleepMode_QuitTalking");

            } else {
                // 关闭 wifi，进入待机模式
                Application::GetInstance().EnterSleepMode();
            }
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGE(TAG, "退出休眠模式");
        });
        power_save_timer_->OnShutdownRequest([this]() {
            // 关机
            if (IsCharging()) {
                // 充电模式下不管
            } else {
                PowerOff();
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    virtual void ResetPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->ResetTimer();
        }
    };

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(true);
            power_save_timer_->WakeUp();
        }
    };
    // SPI初始化
    void InitializeSpi() {
        spi_bus_config_t buscfg = {
            .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
            .miso_io_num = -1,  // No MISO for this display
            .sclk_io_num = DISPLAY_SPI_SCLK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DISPLAY_WIDTH * 40 * sizeof(uint16_t),  // Reduced for power saving
        };
        
        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        }
    }

    // ST7789W3初始化 (240x296)
    void InitializeSt7789Display() {
        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = DISPLAY_SPI_CS_PIN,
            .dc_gpio_num = DISPLAY_SPI_DC_PIN,
            .spi_mode = 0,
            .pclk_hz = DISPLAY_SPI_SCLK_HZ,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel IO creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
            .rgb_ele_order = DISPLAY_RGB_ORDER,
            .bits_per_pixel = 16,
        };
        
        esp_lcd_panel_handle_t panel = nullptr;
        ret = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_reset(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_init(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
            return;
        }

        // Set column/row gap to align visible window and avoid bottom artifacts
        ret = esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel set gap failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Invert colors for ST7789W3
        ret = esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel color invert failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Mirror display
        ret = esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel mirror failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Turn on display
        ret = esp_lcd_panel_disp_on_off(panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel display on failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // display_ = new EyeDisplayHorizontalEmo(panel_io, panel,
        //     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
        //     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
        //     &qrcode_img,
        //     {
        //         .text_font = &font_puhui_20_4,
        //         .icon_font = &font_awesome_20_4,
        //         .emoji_font = font_emoji_64_init(),
        //     });
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH,
            DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
            DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,{
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),
            });
    }

    int MaxBacklightBrightness() {
        return 100;
    }

    void InitializeChargingGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << STANDBY_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        gpio_config_t io_conf2 = {
            .pin_bit_mask = (1ULL << CHARGING_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf2));
    }

    // 初始化耳机检测GPIO
    void InitializeHeadphoneDetection() {
        // 配置HPR-SIGN为输入（检测耳机插入）
        gpio_config_t hpr_conf = {
            .pin_bit_mask = (1ULL << HPR_SIGN_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&hpr_conf));

        // 配置MCU MUTE为输出（控制静音）
        gpio_config_t mute_conf = {
            .pin_bit_mask = (1ULL << MCU_MUTE_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&mute_conf));

        gpio_set_level(MCU_MUTE_PIN, 0);

        // 初始化时读取一次状态并设置MCU MUTE
        UpdateMuteSignal();

        ESP_LOGI(TAG, "Headphone detection GPIO initialized");
    }

    // 更新MCU MUTE信号
    void UpdateMuteSignal() {
        int hpr_level = gpio_get_level(HPR_SIGN_PIN);
        // HPR-SIGN为高时，有耳机插入，输出MCU MUTE为高
        // HPR-SIGN为低时，无耳机插入，输出MCU MUTE为低
        gpio_set_level(MCU_MUTE_PIN, 0);
        ESP_LOGI(TAG, "HPR-SIGN: %d, MCU MUTE: %d", hpr_level, hpr_level);
    }

    // 耳机检测监控任务
    static void HeadphoneDetectionTask(void* arg) {
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        int last_hpr_level = -1;

        for (;;) {
            int current_hpr_level = gpio_get_level(HPR_SIGN_PIN);
            
            // 如果状态发生变化，更新MCU MUTE
            if (current_hpr_level != last_hpr_level) {
                self->UpdateMuteSignal();
                last_hpr_level = current_hpr_level;
            }

            // 每100ms检查一次
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "first_level: %d", first_level);


        boot_button_.OnClick([this]() {

            if (Application::GetInstance().IsTmpFactoryTestMode()) {
                // 通过按键测试
                // display_->UpdateTestItem("key", 1);
                return;
            }


            if (CheckAndHandleEnterSleepMode()) {
                // 交给休眠逻辑托管
                ESP_LOGI(TAG, "长按唤醒");
                return;
            }
            auto& app = Application::GetInstance();
            app.ToggleChatState();
            // display_->TestNextEmotion();
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
                ESP_LOGI(TAG, "执行关机操作");
                // vTaskDelay(pdMS_TO_TICKS(200));
                // auto codec = GetAudioCodec();
                // codec->EnableOutput(true);
                // Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                this->GetBacklight()->SetBrightness(0, false);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    board->display_->SetEmotion("neutral");

                    if (board->IsCharging()) {
                        // 充电中，只关闭背光
                        board->GetBacklight()->SetBrightness(0, false);
                        board->is_charging_sleep_ = true;
                        Application::GetInstance().QuitTalking();
                    } else {
                        // 没有充电，关机
                        board->PowerOff();
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        reset_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "reset_button_.OnLongPress");
            InnerResetWifiConfiguration();
        });

        break_button_.OnPressDown([this]() {
            Application::GetInstance().ToggleChatState();
        });
        
        // Volume up button - short press to increase volume
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume up: %d", volume);
        });
        
        // Volume down button - short press to decrease volume
        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume down: %d", volume);
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker")); 
        thing_manager.AddThing(iot::CreateThing("Screen"));   
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
    int MaxVolume() {
        return 80;
    }

    void InnerResetWifiConfiguration() {
        // 强制拉低背光 io
        // gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        // vTaskDelay(pdMS_TO_TICKS(10));
        if (GetNetworkType() == NetworkType::WIFI) {
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.ResetWifiConfiguration();
        }
    }

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
    }

    virtual bool NeedPlayProcessVoice() override {
        return true;
    }


    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(GPIO_NUM_NC, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);
        

        // 注册充电状态改变回调
        power_manager_->SetChargingStatusCallback([this](bool is_charging) {
            ESP_LOGI(TAG, "充电状态改变: %s", is_charging ? "开始充电" : "停止充电");
            // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
            if (is_charging) {
                // 充电开始时的处理逻辑
                ESP_LOGI(TAG, "检测到开始充电");
                // 降低发热                
                GetBacklight()->SetBrightness(5, false);
                
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");
                
                if (this->is_charging_sleep_) {
                    ESP_LOGI(TAG, "充电停止，关机");
                    PowerOff();
                }
            }

            // 通知 mqtt 
            auto& mqtt_client = MqttClient::getInstance();
            mqtt_client.ReportTimer();

        });
    }

    
    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }
public:
    MovecallMojiESP32S3() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1, UART_NUM_2),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        reset_button_(RESET_BUTTON_GPIO),
        break_button_(BREAK_BUTTON_GPIO),
        audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO) { 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);

        // 设置I2C master日志级别为ERROR，忽略I2C事务失败的日志
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        InitializeHeadphoneDetection();
        InitializeChargingGpio();
        InitializeGpio(POWER_GPIO, true);
        InitializeGpio(ML307_EN, true);
        InitializeSpi();
        InitializeSt7789Display();
        
        InitializeButtons();
        InitializeIot();
        InitializePowerManager();
        InitializePowerSaveTimer();
        // ESP_LOGI(TAG, "ReadADC2_CH1_Oneshot");
        // ReadADC2_CH1_Oneshot();
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
        }

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            auto& app = Application::GetInstance();
            
            // 如果是静默启动状态，忽略唤醒词
            if (app.IsSilentStartup()) {
                ESP_LOGI(TAG, "静默启动状态，忽略唤醒词: %s", command.c_str());
                return;
            }
            
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", app.GetDeviceState());
                // if(app.GetDeviceState() != kDeviceStateListening){
                // }
                app.WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                InnerResetWifiConfiguration();
            }
        });
        xTaskCreate(
            RestoreBacklightTask,      // 任务函数
            "restore_backlight",       // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );

        // 启动耳机检测监控任务
        xTaskCreate(
            HeadphoneDetectionTask,    // 任务函数
            "headphone_detect",        // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );

        if (Application::GetInstance().IsTmpFactoryTestMode()) {
            // display_->EnterTestMode();
            // display_->SetTestItems(test_items);
            // 开始产测模式

            Application::GetInstance().Schedule([this]() {
                display_->StartRGBTest();
                vTaskDelay(pdMS_TO_TICKS(9000));
                display_->StopRGBTest();
            }, "factory_test_mode");

            Application::GetInstance().Schedule([this]() {
                // 尝试连接产测路由wifi
                auto& wifi_station = WifiStation::GetInstance();
                wifi_station.Start();

                ESP_LOGI(TAG, "产测模式临时连接产测路由器");
                auto& wifi_manager = WifiConnectionManager::GetInstance();
                esp_err_t ret = wifi_manager.Connect(CONFIG_PRODUCT_TEST_WIFI, CONFIG_PRODUCT_TEST_WIFI_PASSWORD);
                ESP_LOGI(TAG, "产测模式临时连接产测路由器 ret: %d", ret);
                // if (ret == ESP_OK) {
                //     display_->UpdateTestItemStatus("wifi", 1);
                // } else {
                //     // 设置失败
                //     display_->UpdateTestItemStatus("wifi", 2);
                // }
            }, "factory_test_mode");

            Application::GetInstance().Schedule([this]() {
                // ADC 电池检测
                vTaskDelay(pdMS_TO_TICKS(1000));
                int level = 0;
                bool charging = false;
                bool discharging = false;
                GetBatteryLevel(level, charging, discharging);
                // 合理范围：1..100 认为有效（0 可能意味着未接电池/异常）
                // if (level >= 1 && level <= 100) {
                //     display_->UpdateTestItemStatus("battery", 1);
                // } else {
                //     display_->UpdateTestItemStatus("battery", 2);
                // }
            }, "adc_test");
        }
    }

    virtual void PowerOff() override {
        gpio_set_level(POWER_GPIO, 0);
    }

    virtual void WakeWordDetected() override {
        ESP_LOGI(TAG, "WakeWordDetected");
        // display_->UpdateTestItemStatus("mic", 1);

        GetAudioCodec()->EnableOutput(true);
        Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    bool CheckAndHandleEnterSleepMode() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSleeping) {
            // 如果休眠中
            app.ExitSleepMode();
            return true;
        }
        return false;
    }

    static void RestoreBacklightTask(void* arg) {
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        int level;
        bool charging, discharging;
        self->GetBatteryLevel(level, charging, discharging);
        // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(self->GetDisplay());
        self->GetBacklight()->RestoreBrightness();

        vTaskDelete(NULL); // 任务结束时删除自己
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual int GetPeriod() override { 
        return 1; 
    }
    
    virtual int GetMaxFrameNum() override { 
        return 17;
    }


    virtual bool IsCharging() override {
        int chrg = gpio_get_level(CHARGING_PIN);
        int standby = gpio_get_level(STANDBY_PIN);
        // return false;
        return chrg == 0 || standby == 0;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
        return true;
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(MovecallMojiESP32S3);