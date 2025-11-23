#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "led/gpio_led.h"
#include "led/single_led.h"
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
#include "lvlin_data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "driver/ledc.h"
#include "led_signal.h"
#include "power_manager.h"
#include <esp_timer.h>
#include <inttypes.h>

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 5
#define SLEEP_TIME_SEC 60 * 3
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    // Button collision_button;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    uint32_t power_on_time_;  // 上电时间戳
    bool sleep_flag_ = false;
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人", "你好冬冬"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, SLEEP_TIME_SEC, portMAX_DELAY);  // peter mark 休眠时间
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto& application = Application::GetInstance();
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "Sleep mode");
            PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Shutting down");
        });
        power_save_timer_->OnShutdownRequest([this]() {
            
        });
        power_save_timer_->SetEnabled(true);
    }


    void LongPressSleepCheck(int first_level) {
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
            ESP_LOGI(TAG, "Long press");
            sleep_flag_ = true;
            auto& application = Application::GetInstance();
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
        }
    }

    void InitializeButtons() {

        const int chat_mode = Application::GetInstance().GetChatMode();
        rec_button_ = new Button(BUILTIN_REC_BUTTON_GPIO);

        static int rec_first_level = gpio_get_level(BUILTIN_REC_BUTTON_GPIO);
        static int boot_first_level = gpio_get_level(BOOT_BUTTON_GPIO);

        if (chat_mode == 0) {
            rec_button_->OnPressUp([this]() {
                // 检查是否已经过了5秒
                if ((esp_timer_get_time() / 1000 - power_on_time_) < 5000) {
                    return;
                }
                
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
                if (Application::GetInstance().IsTmpFactoryTestMode()) {
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
                    return;
                }
                
                WakeUpPowerSaveTimer();
                // 检查是否已经过了5秒
                if ((esp_timer_get_time() / 1000 - power_on_time_) < 5000) {
                    return;
                }
                
                auto &app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StartListening();
            });
        } else {
            rec_button_->OnPressDown([this]() {

                if (Application::GetInstance().IsTmpFactoryTestMode()) {
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
                    return;
                }
                WakeUpPowerSaveTimer();
                
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
            boot_button_.OnClick([this]() {

                if (Application::GetInstance().IsTmpFactoryTestMode()) {
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
                    return;
                }
                WakeUpPowerSaveTimer();
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
        }

        boot_button_.OnLongPress([this, boot_first_level]() {
            LongPressSleepCheck(boot_first_level);
        });
        rec_button_->OnLongPress([this, rec_first_level]() {
            // 要忽略上电的时候首次长按
            // 长按只播音频，并设置 flag，松手断电
            LongPressSleepCheck(rec_first_level);
        });

        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
        rec_button_->OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "rec_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });

        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "Press up");
            if(sleep_flag_){
                sleep_flag_ = false;
                // 检查是否在充电状态
                bool is_charging = PowerManager::GetInstance().IsCharging();
                if (!is_charging) {
                    // 电池模式下，等待音频播放完成后再关机
                    ESP_LOGI(TAG, "等待音频播放完成");
                    int wait_count = 0;
                    while (!Application::GetInstance().GetAudioService().IsIdle() && wait_count < 80) {
                        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms检查一次，最多等待4秒
                        wait_count++;
                    }
                    // 额外等待一小段时间，确保音频完全播放完毕
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "音频播放完成，准备关机");
                    // 提前停止所有功能，加快关机速度
                    Application::GetInstance().QuitTalking();
                } else {
                    // 充电模式下，禁用定时器，避免定时器再次触发休眠
                    if (power_save_timer_) {
                        power_save_timer_->SetEnabled(false);
                        ESP_LOGI(TAG, "充电模式下长按关机，禁用PowerSaveTimer");
                    }
                }
                PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
            }
        });
        rec_button_->OnPressUp([this]() {
            ESP_LOGI(TAG, "Press up");
            if(sleep_flag_){
                sleep_flag_ = false;
                // 检查是否在充电状态
                bool is_charging = PowerManager::GetInstance().IsCharging();
                if (!is_charging) {
                    // 电池模式下，等待音频播放完成后再关机
                    ESP_LOGI(TAG, "等待音频播放完成");
                    int wait_count = 0;
                    while (!Application::GetInstance().GetAudioService().IsIdle() && wait_count < 80) {
                        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms检查一次，最多等待4秒
                        wait_count++;
                    }
                    // 额外等待一小段时间，确保音频完全播放完毕
                    vTaskDelay(pdMS_TO_TICKS(100));
                    ESP_LOGI(TAG, "音频播放完成，准备关机");
                    // 提前停止所有功能，加快关机速度
                    Application::GetInstance().QuitTalking();
                } else {
                    // 充电模式下，禁用定时器，避免定时器再次触发休眠
                    if (power_save_timer_) {
                        power_save_timer_->SetEnabled(false);
                        ESP_LOGI(TAG, "充电模式下长按关机，禁用PowerSaveTimer");
                    }
                }
                PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
            }
        });
    }

    void InitializeLedSignal() {
        LedSignal::GetInstance().MonitorAndUpdateLedState_timer();
    }

    void SetLedBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
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
        PowerManager::GetInstance();
    }

    void InitializeDataPointManager() {
        // 设置 LvlinDataPointManager 的回调函数
        LvlinDataPointManager::GetInstance().SetCallbacks(
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
            [this](int value) { SetBrightness(value); },
            [this]() -> int { return GetSpeed_(); },
            [this](int value) { SetSpeed(value); }
        );
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
        power_on_time_ = esp_timer_get_time() / 1000;  // 记录上电时间（毫秒）

        InitializePowerManager();
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        // 如果是从深度睡眠被碰撞 GPIO 唤醒，则先等待稳定摇晃，否则重新睡眠
        // WaitForCollisionShakeOrSleepIfWokenByCollision();

        // 先初始化 PowerSaveTimer，因为按钮回调可能会立即调用它
        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();

        if (s_factory_test_mode == 0) {
            InitializeLedSignal();
            InitializeButtons();
        }

        
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(BUILTIN_LED_GPIO, 0);

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();



        audio_codec.OnWakeUp([this](const std::string& command) {
            WakeUpPowerSaveTimer();
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

        PowerManager::GetInstance().CheckBatteryStatusImmediately();

        // 注册充电状态变化回调，处理拔掉USB后的自动关机
        PowerManager::GetInstance().SetChargingStateChangeCallback(
            [this](bool was_charging, bool is_charging) {
                // 检测到从充电变为非充电（拔掉USB）
                if (was_charging && !is_charging) {
                    ESP_LOGI(TAG, "检测到停止充电（拔掉USB）");
                    
                    // 延迟一小段时间确认状态稳定，避免误判
                    Application::GetInstance().Schedule([this]() {
                        // 再次确认不在充电状态
                        if (!PowerManager::GetInstance().IsCharging()) {
                            bool is_in_sleep_mode = (power_save_timer_ && power_save_timer_->IsInSleepMode());
                            // 如果设备处于睡眠状态，自动关机
                            if (is_in_sleep_mode) {
                                ESP_LOGI(TAG, "设备处于睡眠状态且已拔掉USB，自动关机");
                                // 等待音频播放完成后再关机
                                int wait_count = 0;
                                while (!Application::GetInstance().GetAudioService().IsIdle() && wait_count < 80) {
                                    vTaskDelay(pdMS_TO_TICKS(50));
                                    wait_count++;
                                }
                                vTaskDelay(pdMS_TO_TICKS(100));
                                // 提前停止所有功能，加快关机速度
                                Application::GetInstance().QuitTalking();
                                PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
                            }
                        }
                    }, "AutoPowerOffAfterUnplug");
                }
            }
        );

        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        InitializeDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");
    }

    virtual void WakeUpPowerSaveTimer() {
        sleep_flag_ = false;
        if (power_save_timer_) {
            // 检测定时器是否已启用，如果没有开启就打开
            power_save_timer_->SetEnabled(true);
            power_save_timer_->WakeUp();
            ESP_LOGI(TAG, "唤醒定时器：确保定时器已启用并唤醒");
        }
    };

    virtual int GetBatteryCheckTimeOffset() override { return 60; }


    virtual bool NeedSilentStartup() override {
        return false;
    }

    // 低电量是否阻止启动（低电量时直接关机）
    bool NeedBlockLowBattery() override {
        return true;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    virtual bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    int GetDefaultChatMode() override {
        return 1;
    }

    virtual bool NeedPlayProcessVoiceWithLife() override {
        // 自然对话也要播放提示音
        return true;
    }

    void EnterDeepSleepIfNotCharging() {
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    // 设备关机方法（低电量时调用）
    virtual void PowerOff() override {
        ESP_LOGI(TAG, "PowerOff called (低电量关机)");
        
        // 检查充电状态
        bool is_charging = PowerManager::GetInstance().IsCharging();
        if (is_charging) {
            // 充电中，只断开 socket，不进入深度睡眠
            ESP_LOGI(TAG, "充电中，只断开连接");
            Application::GetInstance().QuitTalking();
            return;
        }
        
        // 电池模式下，确保音频输出已启用，然后等待低电量提示音播放完成后再关机
        auto codec = GetAudioCodec();
        if (codec) {
            codec->EnableOutput(true);
            ESP_LOGI(TAG, "已启用音频输出，等待低电量提示音播放完成");
        }
        
        // 给一点时间让音频包放入队列并开始播放
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // 等待音频播放完成（队列为空）
        int wait_count = 0;
        while (!Application::GetInstance().GetAudioService().IsIdle() && wait_count < 80) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 50ms检查一次，最多等待4秒
            wait_count++;
        }
        // 额外等待一小段时间，确保音频完全播放完毕
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "低电量提示音播放完成，准备关机");
        
        // 停止所有功能
        Application::GetInstance().QuitTalking();
        
        // 电池模式下，进入深度睡眠
        ESP_LOGI(TAG, "电池模式下低电量，进入深度睡眠");
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    void SetPowerSaveTimer(bool enable) {
        power_save_timer_->SetEnabled(enable);
    }

    PowerSaveTimer* GetPowerSaveTimer() override {
        return power_save_timer_;
    }

    uint8_t GetBrightness() {
        return LedSignal::GetInstance().GetBrightness();
    }
    
    void SetBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
    }

    uint8_t GetDefaultBrightness() {
        return LedSignal::GetInstance().GetDefaultBrightness();
    }

    // 语速相关方法
    int GetSpeed_() {
        // 从设置中获取语速，默认值为0（对应正常语速）
        Settings settings("wifi", true);
        return settings.GetInt("speed", 50);
    }
    int GetVoiceSpeed() {
        int speed = GetSpeed_();
        return speed - 50;
    }
    void SetSpeed(int speed) {
        // 限制语速值在有效范围内 (0-200, 对应-50%到150%)
        int clamped_speed = std::max(0, std::min(200, speed));
        Settings settings("wifi", true);
        settings.SetInt("speed", clamped_speed);
        ESP_LOGI(TAG, "Speed set to: %d", clamped_speed);

        MqttClient::getInstance().GetRoomInfo(false);
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return LvlinDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return LvlinDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, uint32_t& value) const override {
        return LvlinDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, uint32_t value) override {
        return LvlinDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        LvlinDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, uint32_t value) override {
        LvlinDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

    void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) override {
        // LvlinDataPointManager 目前没有 binary 数据点，但保持接口一致性
        ESP_LOGW(TAG, "ProcessBinaryDataPointValue called for %s but LvlinDataPointManager doesn't support binary data points", name.c_str());
    }

};

DECLARE_BOARD(CustomBoard);
