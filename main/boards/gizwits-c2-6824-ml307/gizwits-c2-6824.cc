#include "dual_network_board.h"
#include "wifi_board.h"

#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "led/gpio_led.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include <esp_sleep.h>
#include "power_manager.h"
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
#include <vector>
#include <string>

#define TAG "CustomBoard"

class CustomBoard : public DualNetworkBoard {
private:
    bool is_sleep_ = false;
    int64_t power_on_time_ = 0;  // 记录上电时间
    bool need_power_off_ = false;

    Button boot_button_;
    Button* power_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人","你好冬冬"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 2, portMAX_DELAY);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Shutting down");
            PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            
        });
        power_save_timer_->SetEnabled(true);
    }

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();
    }



    virtual void RunResetWifiConfiguration() {
        if (GetNetworkType() == NetworkType::WIFI) {
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.ResetWifiConfiguration();
        }
    }



    void InitializePowerManager() {
        PowerManager::GetInstance();
    }
    void PowerOff() {
        ESP_LOGI(TAG, "PowerOff called, setting POWER_HOLD_GPIO low");
        gpio_set_level(POWER_HOLD_GPIO, 0);
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);


        const int chat_mode = Application::GetInstance().GetChatMode();
        power_button_ = new Button(BUILTIN_POWER_BUTTON_GPIO);

        power_button_->OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
            auto& app = Application::GetInstance();
            
            // 在写入NVS前，先通过ADC刷新一次充电状态
            bool is_charging_now = false;
            // 快速多次刷新，提升判定稳定性
            for (int i = 0; i < 3; ++i) {
                PowerManager::GetInstance().CheckBatteryStatusImmediately();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            is_charging_now = PowerManager::GetInstance().IsCharging();
            ESP_LOGI(TAG, "长按键操作前充电状态: %s", is_charging_now ? "充电中" : "未充电");
            
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
                vTaskDelay(pdMS_TO_TICKS(2000));
                need_power_off_ = true;
            }
        });

        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            app.ToggleChatState();
        });

        power_button_->OnClick([this]() {
            auto &app = Application::GetInstance();
            app.ToggleChatState();
        });

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

    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

public:
    CustomBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1, UART_NUM_0),boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
    
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << POWER_HOLD_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(POWER_HOLD_GPIO, 1);

        InitializePowerManager();
        InitializePowerSaveTimer();       
        InitializeButtons();
        InitializeIot();

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                RunResetWifiConfiguration();
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

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(CustomBoard);
