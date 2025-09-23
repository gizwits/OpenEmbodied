#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "power_manager.h"
#include "assets/lang_config.h"
#include "font_awesome_symbols.h"
#include "wifi_connection_manager.h"
#include "settings.h"
#include <esp_lcd_st7735s.h>
#include "data_point_manager.h"
#include "led/single_led.h"
#include "display/eye_display_horizontal_emojis.h"
#include "display/display.h"
#include <esp_lvgl_port.h>

#include <wifi_station.h>
#include "power_save_timer.h"
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include <esp_system.h>

#include <math.h>

#define TAG "MovecallMojiESP32S3"

#define SLEEP_TIME_SEC 60 * 10


LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);


class MovecallMojiESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    
    Button power_button_;
    VbAduioCodec audio_codec;
    EyeDisplayHorizontalEmo* display_;
    bool need_power_off_ = false;
    int64_t power_on_time_ = 0;  // 记录上电时间
    PowerManager* power_manager_;
    TickType_t last_touch_time_ = 0;  // 上次抚摸触发时间
    PowerSaveTimer* power_save_timer_;
    bool is_charging_sleep_ = false;

    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};


    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

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



    void EnterDeepSleepIfNotCharging() {
        power_manager_->EnterDeepSleepIfNotCharging();
    }
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

    
    // ST7735S初始化
    void InitializeST7735SDisplay() {
        // 上电先关闭背光
        gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
#if DISPLAY_BACKLIGHT_OUTPUT_INVERT
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 1);
#else
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
#endif

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
            .rgb_endian = LCD_RGB_ENDIAN_RGB,
            .bits_per_pixel = 16,
        };
        
        esp_lcd_panel_handle_t panel = nullptr;
        ret = esp_lcd_new_panel_st7735s(panel_io, &panel_config, &panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // 按手册保证 reset 低保持与上电稳定时间
        vTaskDelay(pdMS_TO_TICKS(10));
        ret = esp_lcd_panel_reset(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
            return;
        }
        // 复位后等待面板内部稳定
        vTaskDelay(pdMS_TO_TICKS(120));
        ret = esp_lcd_panel_init(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
            return;
        }

        // 恢复 gap 为 0，避免错误偏移导致角落未覆盖
        esp_lcd_panel_set_gap(panel, 0, 0);

        // 恢复反色设置为 true，避免背景变白
        ret = esp_lcd_panel_invert_color(panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel color invert failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Mirror display if needed
        ret = esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel mirror failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // 打开显示，但仍保持背光关闭
        ret = esp_lcd_panel_disp_on_off(panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel display on failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // 创建显示对象
        DisplayFonts fonts = { .text_font = &font_puhui_20_4, .icon_font = nullptr, .emoji_font = nullptr };
        display_ = new EyeDisplayHorizontalEmo(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            fonts);

        // 让 LVGL 完成首次界面创建并强制刷新 1~2 帧
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        // 背光控制完全交给PWM，不在这里手动控制GPIO
    }

    int MaxBacklightBrightness() {
        return 8;
    }

    void InitializeChargingGpio() {
        // gpio_config_t io_conf = {
        //     .pin_bit_mask = (1ULL << STANDBY_PIN),
        //     .mode = GPIO_MODE_INPUT,
        //     .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
        //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
        //     .intr_type = GPIO_INTR_DISABLE
        // };
        // ESP_ERROR_CHECK(gpio_config(&io_conf));

        // gpio_config_t io_conf2 = {
        //     .pin_bit_mask = (1ULL << CHARGING_PIN),
        //     .mode = GPIO_MODE_INPUT,
        //     .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
        //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
        //     .intr_type = GPIO_INTR_DISABLE
        // };
        // ESP_ERROR_CHECK(gpio_config(&io_conf2));
    }

    void InitializeButtons() {
        // BOOT 按键事件：短按、长按、松开打印
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "BOOT 按键短按");
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT 按键长按，进入配网");
            ResetWifiConfiguration();
        });
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "BOOT 按键松开");
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
        ResetWifiConfiguration();
    }

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
    }

    virtual bool NeedPlayProcessVoice() override {
        return true;
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

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_NC, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);
        // 注册充电状态改变回调
        power_manager_->SetChargingStatusCallback([this](bool is_charging) {
            ESP_LOGI(TAG, "充电状态改变: %s", is_charging ? "开始充电" : "停止充电");
            // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
            if (is_charging) {
                // 充电开始时的处理逻辑
                ESP_LOGI(TAG, "检测到开始充电");
                // 降低发热                
                // GetBacklight()->SetBrightness(5, false);
                
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");
                
                ESP_LOGI(TAG, "检测到停止充电");
                auto state = Application::GetInstance().GetDeviceState();
                // 待机状态，直接关机
                if (state == kDeviceStateIdle) {
                    PowerOff();
                }
            }

            Application::GetInstance().Schedule([this]() {
                // 通知 mqtt 
                auto& mqtt_client = MqttClient::getInstance();
                mqtt_client.ReportTimer();
            });

        });
    }


    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();

        EnterDeepSleepIfNotCharging();
    }

    void initPowerButton() {
        static int first_level = gpio_get_level(POWER_BUTTON_GPIO);
        ESP_LOGI(TAG, "initPowerButton");
        power_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "power_button_.OnPressDown");
            auto& app = Application::GetInstance();
            app.ToggleChatState();
            // 按键切换表情
            if (display_) {
                display_->TestNextEmotion();
            }

        });
        
        power_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "power_button_.OnLongPress");
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
                if (!is_charging_sleep_) {
                    ESP_LOGI(TAG, "执行关机操作");
                    Application::GetInstance().QuitTalking();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    auto codec = GetAudioCodec();
                    codec->EnableOutput(true);
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                    need_power_off_ = true;
                }
                
            }
        });
        power_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "power_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    board->run_sleep_mode(false);

                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });
    }


public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), power_button_(POWER_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO) { 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);

        // 设置I2C master日志级别为ERROR，忽略I2C事务失败的日志
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        
        InitializeGpio(POWER_HOLD_GPIO, true);

        InitializeSpi();
        InitializeST7735SDisplay();

        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            initPowerButton();
        }

        InitializeIot();
        InitializePowerManager();
        InitializeDataPointManager();
        InitializePowerSaveTimer();

        // 注册关机回调，确保任何重启路径都会先关闭背光
        esp_register_shutdown_handler([](){
            ESP_LOGI(TAG, "Shutdown handler: TurnOffBacklight before restart");
            gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        });

        if (power_manager_) {
            ESP_LOGI(TAG, "Before CheckBatteryStatusImmediately");
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "After CheckBatteryStatusImmediately, battery: %d", power_manager_->GetBatteryLevel());
        }
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

        xTaskCreate(
            RestoreBacklightTask,      // 任务函数
            "restore_backlight",       // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );
    }

    virtual void PowerOff() override {
        gpio_set_level(POWER_HOLD_GPIO, 0);
    }

    // virtual void WakeWordDetected() override {
    //     ESP_LOGI(TAG, "WakeWordDetected");

    //     GetAudioCodec()->EnableOutput(true);
    //     Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
    // }

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

        // xunguan_display->StartAutoTest(1000);

        // if (charging) {
        //     // 降低发热            
        //     // xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::POWER_SAVE);
        //     xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);
        // } else {
        //     xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);
        // }
        vTaskDelete(NULL); // 任务结束时删除自己
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool IsCharging() override {
        return power_manager_->IsCharging();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        // ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
        return true;
    }
    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }



    void SetPowerSaveTimer(bool enable) {
        power_save_timer_->SetEnabled(enable);
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

DECLARE_BOARD(MovecallMojiESP32S3);