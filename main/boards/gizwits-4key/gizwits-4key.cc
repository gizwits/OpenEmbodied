#include "wifi_board.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "settings.h"
#include "power_save_timer.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include "led/circular_strip.h"
#include <esp_timer.h>
#include "power_manager.h"
#include "assets/lang_config.h"
#include "data_point_manager.h"
#include <esp_system.h>
#include "server/giz_mqtt.h"

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
    PowerSaveTimer* power_save_timer_;


    int64_t rec_last_click_time_ = 0;
    int64_t power_on_time_ = 0;  // 记录上电时间
    
    // 双按钮长按检测相关变量
    bool volume_up_long_pressed_ = false;
    bool volume_down_long_pressed_ = false;
    int64_t dual_long_press_time_ = 0;
    bool is_charging_sleep_ = false;
    bool is_sleep_ = false;
    
    bool last_charging_state_ = false;  // 跟踪上一次充电状态，用于检测充电状态变化
    
    // 静默启动标志，在构造函数中设置，供Application::Start()读取
    static bool silent_startup_from_board_;


    void InitializePowerSaveTimer() {
        // 20 分钟进休眠
        // 30 分钟 关机
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 5, 60 * 6);
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
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，短按 BOOT 不执行操作");
                return;
            }
            WakeUp();
        }); 
        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat");
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，忽略重复按键");
                return;
            }
            if(count >= 5){
                ResetWifiConfiguration();
            } else {
                Application::GetInstance().ToggleChatState();
            }
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
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
            
            // 只有充电导致的静默启动才需要长按唤醒，异常重启的静默启动直接执行关机
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，长按清除静默标志并重启");
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
            
            // 提前播放音频
            // 非休眠模式才播报
            if (!is_sleep_) {
                ESP_LOGI(TAG, "执行关机操作");
                Application::GetInstance().QuitTalking();
                vTaskDelay(pdMS_TO_TICKS(200));
                auto codec = GetAudioCodec();
                codec->EnableOutput(true);
                Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // NVS标志已经在OnLongPress中设置了，这里直接关机
                // 检查是否在充电状态
                bool is_charging = power_manager_ && power_manager_->IsCharging();
                if (is_charging) {
                } else {
                    PowerOff();
                }
            }
        });

        auto chat_mode = Application::GetInstance().GetChatMode();
        ESP_LOGI(TAG, "chat_mode: %d", chat_mode);

        if (chat_mode == 0) {
            rec_button_.OnPressDown([this]() {
                // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，短按 REC 不执行操作");
                    return;
                }
                WakeUp();
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                Application::GetInstance().StartListening();
            });
            rec_button_.OnPressUp([this]() {
                // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，松开 REC 不执行操作");
                    return;
                }
                ESP_LOGI(TAG, "rec_button_.OnPressUp");
                Application::GetInstance().StopListening();
            });
        } else {
            rec_button_.OnPressDown([this]() {
                // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，短按 REC 不执行操作");
                    return;
                }
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                WakeUp();
                Application::GetInstance().ToggleChatState();
            });
        }
        
        volume_up_button_.OnPressDown([this]() {
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，短按音量+不执行操作");
                return;
            }
            WakeUp();
            ESP_LOGI(TAG, "volume_up_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            codec->SetOutputVolume(volume);
        });
        volume_up_button_.OnLongPress([this]() {
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，长按音量+不执行操作");
                return;
            }
            ESP_LOGI(TAG, "volume_up_button_.OnLongPress");
            CheckDualLongPress();
        });

        volume_down_button_.OnPressDown([this]() {
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，短按音量-不执行操作");
                return;
            }
            WakeUp();
            ESP_LOGI(TAG, "volume_down_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            codec->SetOutputVolume(volume);
        });
        volume_down_button_.OnLongPress([this]() {
            // 只有充电导致的静默启动才禁用按键，异常重启的静默启动允许按键工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，长按音量-不执行操作");
                return;
            }
            ESP_LOGI(TAG, "volume_down_button_.OnLongPress");
            CheckDualLongPress();
        });

    }

    void PowerOff() {
        ESP_LOGI(TAG, "PowerOff called, setting POWER_HOLD_GPIO low");
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

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
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

    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(CHARGING_PIN, STANDBY_PIN, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);

            power_manager_->SetChargingStatusCallback([this](bool is_charging) {
                ESP_LOGI(TAG, "充电状态改变: %s", is_charging ? "开始充电" : "停止充电");
                bool was_charging = last_charging_state_;
                static int64_t last_state_change_time = 0;
                const int64_t DEBOUNCE_TIME_MS = 1000; // 1秒防抖时间
                
                int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
                // 防抖处理：状态变化后1秒内不处理新的变化
                if (was_charging != is_charging && 
                    (last_state_change_time == 0 || (current_time - last_state_change_time) > DEBOUNCE_TIME_MS)) {
                    
                    last_charging_state_ = is_charging;  // 更新状态
                    last_state_change_time = current_time;
                    
                    // 只在静默启动状态下检查拔掉充电线的情况
                    if (silent_startup_from_board_) {
                        // 如果从充电变为非充电，且处于静默启动状态，则自动关机
                        if (was_charging && !is_charging) {
                            ESP_LOGI(TAG, "🔋 静默启动状态下检测到USB已拔掉（从充电变为非充电），自动关机以节省功耗");
                            // 保存标志位：电池模式下关机，保存silent_next=0
                            {
                                Settings settings("system", true);
                                settings.SetInt("silent_next", 0);
                                ESP_LOGI(TAG, "电池模式下关机，保存silent_next=0");
                            }
                            // 延迟一小段时间再关机，避免误判
                            xTaskCreate([](void* arg) {
                                vTaskDelay(pdMS_TO_TICKS(2000));  // 等待2秒确认
                                GizwitsDevBoard* board = static_cast<GizwitsDevBoard*>(arg);
                                if (!board->power_manager_->IsCharging() && board->silent_startup_from_board_) {
                                    ESP_LOGI(TAG, "确认USB已拔掉，执行静默关机（不播放音频）");
                                    // 静默启动状态下直接关机，不播放任何音频
                                    auto& app = Application::GetInstance();
                                    app.QuitTalking();
                                    // 拉低电源保持引脚，关闭电池供电
                                    board->PowerOff();
                                    // 延时3秒后进入深度睡眠
                                    vTaskDelay(pdMS_TO_TICKS(3000));
                                    board->run_sleep_mode(false);
                                }
                                vTaskDelete(NULL);
                            }, "auto_poweroff_task", 2048, this, 5, NULL);  // 减小栈大小：2KB足够（等待+关机操作）
                            return;  // 静默模式下拔掉充电线直接返回，不执行后续逻辑
                        }
                    }
                    
                    // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
                    if (is_charging) {
                        // 充电开始时的处理逻辑
                        ESP_LOGI(TAG, "检测到开始充电");
                        // 降低发热                
                        // GetBacklight()->SetBrightness(5, false);
                        
                    } else {
                        // 充电停止时的处理逻辑（非静默模式）
                        ESP_LOGI(TAG, "检测到停止充电");
                        
                        // 拔掉USB时，清除静默标志，让下次在电池模式下正常启动
                        Settings settings("system", true);
                        settings.SetInt("silent_next", 0);
                        ESP_LOGI(TAG, "拔掉USB，清除NVS silent_next标志");
                        
                        auto state = Application::GetInstance().GetDeviceState();
                        // 待机或休眠状态，直接关机
                        if (state == kDeviceStateIdle || state == kDeviceStateSleeping) {
                            PowerOff();
                        }
                    }
    
                    Application::GetInstance().Schedule([this]() {
                        // 通知 mqtt 
                        auto& mqtt_client = MqttClient::getInstance();
                        mqtt_client.ReportTimer();
                    });
                } else if (was_charging != is_charging) {
                    ESP_LOGI(TAG, "充电状态变化被防抖过滤: %s", is_charging ? "开始充电" : "停止充电");
                }
            });
    }

    void WakeUp() {
        is_sleep_ = false;
        gpio_set_level(BUILTIN_SINGLE_LED_GPIO, 0);
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
        InitializeGpio(BUILTIN_SINGLE_LED_GPIO, false);
        InitializeChargingGpio();
        InitializeI2c();
        InitializeIot();
        InitializePowerManager();
        
        // 立即检测一次充电状态，确保能正确判断，并初始化 last_charging_state_
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            // 等待一下让充电状态稳定
            vTaskDelay(pdMS_TO_TICKS(100));
            power_manager_->CheckBatteryStatusImmediately();
            last_charging_state_ = power_manager_->IsCharging();
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
        
        // 如果是静默启动，确保 LED 关闭
        if (silent_startup_from_board_) {
            gpio_set_level(BUILTIN_SINGLE_LED_GPIO, 0);
            if (GetLed()) {
                GetLed()->TurnOff();
            }
            ESP_LOGI(TAG, "静默启动，关闭 LED");
        }
        
        InitializeDataPointManager();
        InitializePowerSaveTimer();
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
        }
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

    virtual bool IsCharging() override {
        return power_manager_ && power_manager_->IsCharging();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }


    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return DataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return DataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, uint32_t& value) const override {
        return DataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, uint32_t value) override {
        return DataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        DataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, uint32_t value) override {
        DataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

    void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) override {
        DataPointManager::GetInstance().ProcessBinaryDataPointValue(name, data, data_len);
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
bool GizwitsDevBoard::silent_startup_from_board_ = false;

DECLARE_BOARD(GizwitsDevBoard);
