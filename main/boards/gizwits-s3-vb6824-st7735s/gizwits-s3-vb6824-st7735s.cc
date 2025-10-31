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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <math.h>

#define TAG "MovecallMojiESP32S3"

#define SLEEP_TIME_SEC 60 * 3


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
    
    // 静默启动标志，在构造函数中设置，供Application::Start()读取
    static bool silent_startup_from_board_;

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
        // 上电先关闭背光，确保开机时屏幕是黑的
        gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);  // 明确拉低

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

        // 让 LVGL 完成首次界面创建并强制刷新 2~3 帧，确保显示内容完全准备好
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
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
        
        // 不在这里启动背光恢复任务，改到构造函数最后根据静默启动状态决定
    }

    int MaxBacklightBrightness() {
        return 60;
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
            static bool last_charging_state = false;
            static int64_t last_state_change_time = 0;
            const int64_t DEBOUNCE_TIME_MS = 1000; // 1秒防抖时间
            
            int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            // 防抖处理：状态变化后1秒内不处理新的变化
            if (last_charging_state != is_charging && 
                (last_state_change_time == 0 || (current_time - last_state_change_time) > DEBOUNCE_TIME_MS)) {
                
                last_charging_state = is_charging;
                last_state_change_time = current_time;
                
                // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
                if (is_charging) {
                    // 充电开始时的处理逻辑
                    ESP_LOGI(TAG, "检测到开始充电");
                    // 降低发热                
                    // GetBacklight()->SetBrightness(5, false);
                    
                } else {
                    // 充电停止时的处理逻辑
                    ESP_LOGI(TAG, "检测到停止充电");
                    
                    // 拔掉USB时，清除静默标志，让下次在电池模式下正常启动
                    Settings settings("system", true);
                    settings.SetInt("silent_next", 0);
                    ESP_LOGI(TAG, "拔掉USB，清除NVS silent_next标志");
                    
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
            } else if (last_charging_state != is_charging) {
                ESP_LOGI(TAG, "充电状态变化被防抖过滤: %s", is_charging ? "开始充电" : "停止充电");
            }
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
        power_manager_->EnterDeepSleepIfNotCharging();
    }

    void initPowerButton() {
        static int first_level = gpio_get_level(POWER_BUTTON_GPIO);
        ESP_LOGI(TAG, "initPowerButton");
        power_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "power_button_.OnPressDown");
            auto& app = Application::GetInstance();
            
            // 如果是静默启动状态（充电插入），短按不执行任何操作
            if (app.IsSilentStartup()) {
                ESP_LOGI(TAG, "静默启动状态，短按电源键不执行操作");
                return;
            }
    
            // 总是先尝试快速打断（未播报时为空操作），再切换聊天状态
            app.AbortSpeaking(kAbortReasonNone);
            // app.ToggleChatState();
            app.StartListening();
            // 立即启用语音处理，避免因播放缓存而 pending
            app.GetAudioService().EnableVoiceProcessing(true, true);
            
            // 按键切换表情
            // if (display_) {
            //     display_->TestNextEmotion();
            // }

        });
        
        power_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "power_button_.OnLongPress");
            auto& app = Application::GetInstance();
            
            // 在写入NVS前，先通过ADC刷新一次充电状态
            bool is_charging_now = false;
            if (power_manager_) {
                // 快速多次刷新，提升判定稳定性
                for (int i = 0; i < 3; ++i) {
                    power_manager_->CheckBatteryStatusImmediately();
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                is_charging_now = power_manager_->IsCharging();
                ESP_LOGI(TAG, "长按键操作前充电状态: %s", is_charging_now ? "充电中" : "未充电");
            }

            // 计算设备运行时间
            int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            int64_t uptime_ms = current_time - power_on_time_;
            ESP_LOGI(TAG, "设备运行时间: %lld ms", uptime_ms);
            
            // 首次上电5秒内且first_level==0才忽略
            const int64_t MIN_UPTIME_MS = 5000; // 5秒
            if (first_level == 0 && uptime_ms < MIN_UPTIME_MS) {
                first_level = 1;
                ESP_LOGI(TAG, "首次上电5秒内，忽略长按操作");
                return;
            }
            
            // 如果是静默启动状态，清除静默标志并重启（开机）
            if (app.IsSilentStartup()) {
                ESP_LOGI(TAG, "静默启动状态，长按清除静默标志并重启");
                Settings settings("system", true);
                settings.SetInt("silent_next", 0);
                // 设置一个标志，表示用户主动唤醒，下次启动不应该静默
                settings.SetInt("user_wakeup", 1);
                ESP_LOGI(TAG, "记录NVS: silent_next=0, user_wakeup=1（充电状态: %s）", is_charging_now ? "充电中" : "未充电");
                
                // 立即重启，不做任何延迟
                esp_restart();
                return;
            }
            
            // 非静默启动状态，执行关机操作
            // 检查是否在充电状态
            bool is_charging = is_charging_now;
            if (is_charging) {
                // 充电状态下关机，先保存静默标志到NVS
                Settings settings("system", true);
                settings.SetInt("silent_next", 1);
                ESP_LOGI(TAG, "充电状态下关机，先保存静默标志到NVS");
            } else {
                // 电池模式下关机，清除静默标志
                Settings settings("system", true);
                settings.SetInt("silent_next", 0);
                ESP_LOGI(TAG, "电池模式下关机，清除静默标志");
            }
            
            // 播放音频并调用PowerOff关机
            // 非休眠模式才播报
            if (!is_charging_sleep_) {
                ESP_LOGI(TAG, "执行关机操作");
                app.QuitTalking();
                vTaskDelay(pdMS_TO_TICKS(200));
                auto codec = GetAudioCodec();
                codec->EnableOutput(true);
                app.PlaySound(Lang::Sounds::P3_SLEEP);
                
                // 如果是充电状态，立即拉低背光
                if (is_charging) {
                    gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
                    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
                    ESP_LOGI(TAG, "关机前拉低背光GPIO");
                }
                
                need_power_off_ = true;
            }
        });
        power_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "power_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // NVS标志已经在OnLongPress中设置了，这里直接关机
                // 检查是否在充电状态
                bool is_charging = power_manager_ && power_manager_->IsCharging();
                if (is_charging) {
                    // USB充电模式下，直接调用PowerOff重启
                    ESP_LOGI(TAG, "USB充电模式，调用PowerOff重启");
                    PowerOff();
                } else {
                    // 电池模式下，调用run_sleep_mode进入深度睡眠
                    xTaskCreate([](void* arg) {
                        auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                        auto& app = Application::GetInstance();
                        
                        // 等待音频播放完成
                        ESP_LOGI(TAG, "等待音频播放完成");
                        int wait_count = 0;
                        while (!app.GetAudioService().IsIdle() && wait_count < 50) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                            wait_count++;
                        }
                        ESP_LOGI(TAG, "音频播放完成，准备关机");
                        
                        app.SetDeviceState(kDeviceStateIdle);
                        board->run_sleep_mode(false);
                        vTaskDelete(NULL);
                    }, "power_off_task", 4028, this, 10, NULL);
                }
            }
        });
    }


public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), power_button_(POWER_BUTTON_GPIO, false, 1000), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO) { 
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
        
        // 立即检测一次充电状态，确保能正确判断
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            // 等待一下让充电状态稳定
            vTaskDelay(pdMS_TO_TICKS(100));
            power_manager_->CheckBatteryStatusImmediately();
        }
        
        // 检查NVS中的静默启动标志和充电状态
        auto reset_reason = esp_reset_reason();
        ESP_LOGI(TAG, "检查静默启动标志，reset_reason: %d", reset_reason);
        if (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_SW) {
            Settings system_settings("system", false);
            int silent_next = system_settings.GetInt("silent_next", 0);
            int user_wakeup = system_settings.GetInt("user_wakeup", 0);
            ESP_LOGI(TAG, "读取NVS silent_next: %d, user_wakeup: %d", silent_next, user_wakeup);
            
            if (user_wakeup == 1) {
                // 用户主动唤醒，清除标志并正常启动
                Settings system_settings_rw("system", true);
                system_settings_rw.SetInt("user_wakeup", 0);
                ESP_LOGI(TAG, "用户主动唤醒，正常启动");
                silent_startup_from_board_ = false;
            } else if (silent_next == 1) {
                // 清除NVS标志（已消费）
                Settings system_settings_rw("system", true);
                system_settings_rw.SetInt("silent_next", 0);
                ESP_LOGI(TAG, "检测到NVS静默启动标志，设置静默启动");
                silent_startup_from_board_ = true;
            } else if (reset_reason == ESP_RST_POWERON && power_manager_) {
                // 只在首次插入USB（ESP_RST_POWERON）时检查充电状态
                // 增加重试机制确保充电状态检测准确
                bool is_charging = false;
                for (int i = 0; i < 3; i++) {
                    is_charging = power_manager_->IsCharging();
                    if (is_charging) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGI(TAG, "检查充电状态: %d", is_charging);
                if (is_charging) {
                    ESP_LOGI(TAG, "首次插入USB，检测到充电状态，设置静默启动");
                    silent_startup_from_board_ = true;
                }
            }
        }
        ESP_LOGI(TAG, "silent_startup_from_board_ 最终值: %d", silent_startup_from_board_);
        
        // 如果是静默启动，确保背光GPIO保持拉低
        if (silent_startup_from_board_) {
            gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
            ESP_LOGI(TAG, "静默启动，保持背光GPIO拉低");
        } else {
            // 正常启动，启动背光恢复任务
            ESP_LOGI(TAG, "正常启动，启动背光恢复任务");
            xTaskCreate(
                RestoreBacklightTask,      // 任务函数
                "restore_backlight",       // 名字
                4096,                      // 栈大小
                this,                      // 参数传递 this 指针
                5,                         // 优先级
                NULL                       // 任务句柄
            );
        }
        
        InitializeDataPointManager();
        InitializePowerSaveTimer();

        // 定时器开关应在初始化完成后再设置，避免空指针
        if (silent_startup_from_board_) {
            SetPowerSaveTimer(false);
        } else {
            SetPowerSaveTimer(true);
        }

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
                ResetWifiConfiguration();
            }
        });
    }

    virtual void PowerOff() override {
        ESP_LOGI(TAG, "PowerOff called, setting POWER_HOLD_GPIO low");
        
        // 先立即拉低背光GPIO，防止重启时看到表情
        gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        ESP_LOGI(TAG, "PowerOff: 先拉低背光GPIO");
        
        gpio_set_level(POWER_HOLD_GPIO, 0);
        
        // 检查是否在充电状态
        bool is_charging = power_manager_ && power_manager_->IsCharging();
        if (is_charging) {
            // USB充电模式下，需要重启让设备检测NVS标志
            ESP_LOGI(TAG, "USB充电模式，重启设备以检测NVS标志");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        // 电池模式下，直接拉低GPIO就能真正关机，不需要额外操作
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
        
        // 确保显示内容已经完全渲染
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 检查是否是静默启动
        if (self->silent_startup_from_board_) {
            ESP_LOGI(TAG, "静默启动，不亮屏");
            vTaskDelete(NULL);
            return;
        }
        
        int level;
        bool charging, discharging;
        self->GetBatteryLevel(level, charging, discharging);
        
        ESP_LOGI(TAG, "Starting backlight restoration, charging: %d", charging);
        
        // 使用更平滑的背光恢复
        self->GetBacklight()->RestoreBrightness();

        vTaskDelete(NULL); // 任务结束时删除自己
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        // 如果是静默启动，每次都返回nullptr并拉低GPIO
        if (silent_startup_from_board_) {
            gpio_set_direction(DISPLAY_BACKLIGHT_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
            return nullptr;
        }
        
        // 正常启动时才创建PwmBacklight对象
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
    
    // 返回是否需要在充电时静默启动
    virtual bool NeedSilentStartup() override {
        // 检查NVS标志或充电状态
        if (silent_startup_from_board_) {
            return true;
        }
        
        // 检查USB插入或深度睡眠唤醒
        auto reset_reason = esp_reset_reason();
        if (reset_reason == ESP_RST_USB || reset_reason == ESP_RST_DEEPSLEEP) {
            return true;
        }
        
        return false;
    }

};

// 静态成员定义
bool MovecallMojiESP32S3::silent_startup_from_board_ = false;

DECLARE_BOARD(MovecallMojiESP32S3);