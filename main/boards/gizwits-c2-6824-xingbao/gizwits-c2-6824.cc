#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
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
#include <esp_system.h>
#include "data_point_manager.h"
#include "device_state_event.h"
#include "settings.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "power_manager.h"

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 3
#define SLEEP_TIME_SEC 10 * 1
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button power_button_;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    bool need_power_off_ = false;
    int64_t power_on_time_ = 0;  // 记录上电时间
    bool is_sleep_ = false;

    PowerManager* power_manager_;
    
    // LED control
    enum LedMode { kLedSolid, kLedSlowBlink, kLedFastBlink };
    esp_timer_handle_t led_timer_ = nullptr;
    LedMode led_mode_ = kLedSolid;
    int led_logic_level_ = 0; // 0 = ON (active-low), 1 = OFF
    bool thinking_active_ = false;

    // 静默启动：插上USB充电时不上电启动，需长按电源键启动
    static bool silent_startup_from_board_;

    bool IsSilent() const {
        return Application::GetInstance().IsSilentStartup() || silent_startup_from_board_;
    }
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

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

    void EnsureLedTimerCreated() {
        if (led_timer_ != nullptr) return;
        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<CustomBoard*>(arg);
                self->led_logic_level_ = (self->led_logic_level_ == 0) ? 1 : 0;
                gpio_set_level(BUILTIN_LED_GPIO, self->led_logic_level_);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "led_blink_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_timer_));
    }

    void StopLedTimer() {
        if (led_timer_ && esp_timer_is_active(led_timer_)) {
            ESP_ERROR_CHECK(esp_timer_stop(led_timer_));
        }
    }

    void SetLedSolidOn() {
        StopLedTimer();
        led_mode_ = kLedSolid;
        led_logic_level_ = 0; // active-low: 0 means ON
        gpio_set_level(BUILTIN_LED_GPIO, led_logic_level_);
    }

    void SetLedBlink(uint64_t period_ms) {
        EnsureLedTimerCreated();
        StopLedTimer();
        // Ensure we start from ON state for visibility
        led_logic_level_ = 0;
        gpio_set_level(BUILTIN_LED_GPIO, led_logic_level_);
        ESP_ERROR_CHECK(esp_timer_start_periodic(led_timer_, period_ms * 1000));
    }

    void ApplyLedMode(LedMode mode) {
        if (IsSilent()) {
            // 静默启动期间，强制关闭LED且不闪烁
            StopLedTimer();
            led_mode_ = kLedSolid;
            led_logic_level_ = 1; // OFF for active-low
            gpio_set_level(BUILTIN_LED_GPIO, led_logic_level_);
            return;
        }
        if (mode == kLedSolid) {
            SetLedSolidOn();
        } else if (mode == kLedSlowBlink) {
            led_mode_ = kLedSlowBlink;
            SetLedBlink(500); // 0.5s period
        } else {
            led_mode_ = kLedFastBlink;
            SetLedBlink(500); // 0.5s period
        }
    }

    void InitializeDeviceStateEvent() {
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState prev, DeviceState curr) {
                // Speaking has highest priority -> solid on
                if (curr == kDeviceStateSpeaking) {
                    ApplyLedMode(kLedSolid);
                    return;
                }
                // Listening -> slow blink
                if (curr == kDeviceStateListening) {
                    ApplyLedMode(kLedSlowBlink);
                    return;
                }
                // Other states -> solid on
                ApplyLedMode(kLedSolid);
            }
        );
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

    void InitializeButtons() {
        
        boot_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "boot_button_.OnPressDown");
            // 开灯
            gpio_set_level(BUILTIN_LED_GPIO, 0);
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
            // auto &app = Application::GetInstance();
            // app.ToggleChatState();
            ResetWifiConfiguration();
        });

        // boot_button_.OnPressRepeat([this](uint16_t count) {
        //     ESP_LOGI(TAG, "boot_button_.OnPressRepeat: %d", count);
        //     if(count >= RESET_WIFI_CONFIGURATION_COUNT){
        //         ResetWifiConfiguration();
        //     }
        // });
    }

    void initPowerButton() {
        static int first_level = gpio_get_level(POWER_BUTTON_GPIO);
        ESP_LOGI(TAG, "initPowerButton");
        power_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "power_button_.OnPressDown");
            auto& app = Application::GetInstance();
            // 静默启动时，短按不生效
            if (app.IsSilentStartup() || silent_startup_from_board_) {
                ESP_LOGI(TAG, "静默启动，短按无效");
                return;
            }
            // 无条件先打断，再直接进入监听并强制开启语音处理，确保“秒停+立即监听”
            app.AbortSpeaking(kAbortReasonNone);
            app.StartListening();
            app.GetAudioService().EnableVoiceProcessing(true, true);
            // 非静默：进入监听后用统一灯效（慢闪），避免出现可对话但灯不亮
            if (!IsSilent()) {
                ApplyLedMode(kLedSlowBlink);
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
                // 如果为静默启动，长按视为用户主动启动：清除静默标志并重启
                if (app.IsSilentStartup() || silent_startup_from_board_) {
                    Settings settings("system", true);
                    settings.SetInt("silent_next", 0);
                    settings.SetInt("user_wakeup", 1);
                    ESP_LOGI(TAG, "静默启动下长按：清除silent_next并设置user_wakeup，重启");
                    esp_restart();
                    return;
                }
                // 刷新一次充电状态并写入 NVS silent_next 与 S3 一致
                bool is_charging_now = false;
                if (power_manager_) {
                    for (int i = 0; i < 2; ++i) {
                        power_manager_->CheckBatteryStatusImmediately();
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                    is_charging_now = power_manager_->IsCharging();
                    ESP_LOGI(TAG, "长按键操作前充电状态: %s", is_charging_now ? "充电中" : "未充电");
                }
                {
                    Settings settings("system", true);
                    settings.SetInt("silent_next", is_charging_now ? 1 : 0);
                    if (is_charging_now) {
                        ESP_LOGI(TAG, "充电状态下关机，先保存silent_next=1");
                    } else {
                        ESP_LOGI(TAG, "电池模式下关机，保存silent_next=0");
                    }
                }
                // 提前播放音频
                // 非休眠模式才播报
                if (!is_sleep_) {
                    ESP_LOGI(TAG, "执行关机操作");
                    Application::GetInstance().QuitTalking();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    auto codec = GetAudioCodec();
                    gpio_set_level(BUILTIN_LED_GPIO, 1);
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
                // 等待关机提示音播放完成后再关机，行为与 S3 版本一致
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<CustomBoard*>(arg);
                    auto& app = Application::GetInstance();
                    ESP_LOGI(TAG, "等待音频播放完成");
                    int wait_count = 0;
                    while (!app.GetAudioService().IsIdle() && wait_count < 50) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_count++;
                    }
                    ESP_LOGI(TAG, "音频播放完成，准备关机");
                    app.SetDeviceState(kDeviceStateIdle);
                    board->PowerOff();
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });
    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        ESP_LOGI(TAG, "Initializing IoT components...");
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        ESP_LOGI(TAG, "Added IoT component: Speaker");
        thing_manager.AddThing(iot::CreateThing("Led"));
        ESP_LOGI(TAG, "Added IoT component: Led");
        ESP_LOGI(TAG, "IoT components initialization complete.");
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
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");
                auto state = Application::GetInstance().GetDeviceState();
                // 待机状态，直接关机
                if (state == kDeviceStateIdle) {
                    gpio_set_level(POWER_HOLD_GPIO, 0);
                }
            }

            
            Application::GetInstance().Schedule([this]() {
                // 通知 mqtt 
                auto& mqtt_client = MqttClient::getInstance();
                mqtt_client.ReportTimer();
            });

        });
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

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), power_button_(POWER_BUTTON_GPIO, false, 1000), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        // 默认拉高，避免上电过程中可见亮灯；后续根据静默状态再决定
        gpio_set_level(BUILTIN_LED_GPIO, 1);
        led_logic_level_ = 1;


        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);
        

        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();
        InitializeGpio(POWER_HOLD_GPIO, true);


        ESP_LOGI(TAG, "Initializing Buttons...");

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();

        ESP_LOGI(TAG, "Initializing LED Signal...");
        InitializeDeviceStateEvent();
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            initPowerButton();
        }

        ESP_LOGI(TAG, "Initializing Power Manager...");
        InitializePowerManager();
        ESP_LOGI(TAG, "Power Manager initialized.");

        // 立即检测一次充电状态
        power_manager_->CheckBatteryStatusImmediately();

        // 检查开机复位原因与充电状态，决定是否静默启动
        auto reset_reason = esp_reset_reason();
        Settings sys_settings("system", false);
        int silent_next = sys_settings.GetInt("silent_next", 0);
        int user_wakeup = sys_settings.GetInt("user_wakeup", 0);
        if (user_wakeup == 1) {
            Settings sys_settings_rw("system", true);
            sys_settings_rw.SetInt("user_wakeup", 0);
            silent_startup_from_board_ = false;
        } else if (silent_next == 1) {
            Settings sys_settings_rw("system", true);
            sys_settings_rw.SetInt("silent_next", 0);
            silent_startup_from_board_ = true;
        } else if (reset_reason == ESP_RST_POWERON) {
            bool is_charging = power_manager_->IsCharging();
            if (is_charging) {
                silent_startup_from_board_ = true;
            }
        }

        // 静默启动时，禁止点亮内置指示灯，并禁用省电计时器
        if (silent_startup_from_board_) {
            led_logic_level_ = 1; // active-low: 1=OFF
            gpio_set_level(BUILTIN_LED_GPIO, led_logic_level_);
            SetPowerSaveTimer(false);
        } else {
            // 正常启动：恢复到常亮初始态（后续状态机会切换）
            led_logic_level_ = 0;
            gpio_set_level(BUILTIN_LED_GPIO, led_logic_level_);
            SetPowerSaveTimer(true);
        }

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            // 静默启动时忽略唤醒词
            if (Application::GetInstance().IsSilentStartup() || silent_startup_from_board_) {
                return;
            }
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
                gpio_set_level(BUILTIN_LED_GPIO, 0);
            } else if (IsCommandInList(command, network_config_words_)) {
                ResetWifiConfiguration();
            }
        });

        power_manager_->CheckBatteryStatusImmediately();


        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        InitializeDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");
    }

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    };

    bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = power_manager_->GetBatteryLevel();
        charging = power_manager_->IsCharging();
        discharging = !charging;
        return true;
    }

    bool IsCharging() override {
        return power_manager_->IsCharging();
    }

    void EnterDeepSleepIfNotCharging() {
        power_manager_->EnterDeepSleepIfNotCharging();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    // 关机行为与 S3 版本保持一致：拉低保持脚；若在充电，则重启以进入静默/充电逻辑
    virtual void PowerOff() override {
        ESP_LOGI(TAG, "PowerOff called, setting POWER_HOLD_GPIO low");
        gpio_set_level(POWER_HOLD_GPIO, 0);

        bool is_charging = power_manager_ && power_manager_->IsCharging();
        if (is_charging) {
            // 充电场景统一按 S3 行为：重启进入静默充电
            ESP_LOGI(TAG, "USB充电模式，重启设备以检测NVS标志");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        // 电池模式下，直接拉低保持脚即可真正关机
    }

    // 返回是否需要在充电时静默启动
    virtual bool NeedSilentStartup() override {
        return silent_startup_from_board_;
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

DECLARE_BOARD(CustomBoard);

// 静态成员定义
bool CustomBoard::silent_startup_from_board_ = false;
