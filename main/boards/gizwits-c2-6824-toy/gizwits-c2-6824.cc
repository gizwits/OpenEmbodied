#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
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
#include "toy_data_point_manager.h"
#include "settings.h"
#include <esp_timer.h>

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"

#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    VbAduioCodec audio_codec;
    Button volume_up_button_;
    Button volume_down_button_;
    Button prev_button_;
    Button next_button_;
    PowerSaveTimer* power_save_timer_;


    // 上电计数器相关
    Settings power_counter_settings_;
    esp_timer_handle_t power_counter_timer_;
    static constexpr int POWER_COUNT_THRESHOLD = 5;  // 触发阈值
    static constexpr int POWER_COUNT_RESET_DELAY_MS = 2000;  // 2秒后重置

    int64_t prev_last_click_time_ = 0;
    int64_t next_last_click_time_ = 0;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 20, 60 * 30);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Shutting down");
            run_sleep_mode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
        });
        power_save_timer_->OnShutdownRequest([this]() {
            
        });
        power_save_timer_->SetEnabled(true);
    }

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.QuitTalking();
            GetAudioCodec()->EnableOutput(true);
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        vb6824_shutdown();
        vTaskDelay(pdMS_TO_TICKS(200));
        // 杰挺不需要唤醒源
        // esp_deep_sleep_enable_gpio_wakeup(1ULL << BOOT_BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
        
        esp_deep_sleep_start();
    }


    // 定时器回调函数
    static void PowerCounterTimerCallback(void* arg) {
        CustomBoard* board = static_cast<CustomBoard*>(arg);
        board->ResetPowerCounter();
    }

    // 重置上电计数器
    void ResetPowerCounter() {
        power_counter_settings_.SetInt("power_count", 0);
        ESP_LOGI(TAG, "Power counter reset to 0");
    }

    // 检查并处理上电计数
    void CheckPowerCount() {
        int current_count = power_counter_settings_.GetInt("power_count", 0);
        current_count++;
        power_counter_settings_.SetInt("power_count", current_count);
        
        ESP_LOGI(TAG, "Power count: %d", current_count);
        
        if (current_count >= POWER_COUNT_THRESHOLD) {
            ESP_LOGI(TAG, "Power count threshold reached! Triggering event...");
            // 在这里添加你的事件处理逻辑
            // 例如：触发某种特殊模式、发送通知等
            
            // 重置计数器
            ResetPowerCounter();
            auto& wifi_station = WifiStation::GetInstance();
            wifi_station.ClearAuth();
            ResetWifiConfiguration();
        }
        
        // 启动定时器，2秒后重置计数器
        esp_timer_start_once(power_counter_timer_, POWER_COUNT_RESET_DELAY_MS * 1000);
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

    void InitializeDataPointManager() {
        // 设置 ToyDataPointManager 的回调函数
        ToyDataPointManager::GetInstance().SetCallbacks(
            [this]() -> bool { return false; }, // IsCharging - toy 版本可能没有充电功能
            []() -> int { return Application::GetInstance().GetChatMode(); },
            [](int value) { Application::GetInstance().SetChatMode(value); },
            [this]() -> int { 
                return 100; // 固定电量 100%
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
            [this]() -> int { return 100; }, // 固定亮度 100%
            [this](int value) { /* toy 版本可能没有亮度调节 */ }
        );
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    prev_button_(PREV_BUTTON_GPIO), next_button_(NEXT_BUTTON_GPIO),
    power_counter_settings_("power_counter", true) {      

        // 初始化上电计数器定时器
        esp_timer_create_args_t timer_args = {
            .callback = &CustomBoard::PowerCounterTimerCallback,
            .arg = this,
            .name = "power_counter_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_counter_timer_));

        InitializeButtons();
        InitializeIot();
        InitializeDataPointManager();
        InitializePowerSaveTimer();

        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        ESP_LOGI(TAG, "Data Point Manager initialized.");

        InitializeGpio(POWER_GPIO, true);

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(LED_GPIO, 0);

        // 检查上电计数
        CheckPowerCount();

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (command == "你好小智" || command.find("小云") != std::string::npos){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (command == "开始配网") {
                ResetWifiConfiguration();
            }
        });
    }

    ~CustomBoard() {
        if (power_counter_timer_) {
            esp_timer_delete(power_counter_timer_);
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

    // virtual Led* GetLed() override {
    //     static GpioLed led(LED_GPIO);
    //     return &led;
    // }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return ToyDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return ToyDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return ToyDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return ToyDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        ToyDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        ToyDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

};

DECLARE_BOARD(CustomBoard);
