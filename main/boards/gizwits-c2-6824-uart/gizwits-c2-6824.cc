#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include <esp_sleep.h>
#include "power_save_timer.h" // retained for other boards; this board uses a lighter timer
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include <wifi_station.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "power_manager.h"
#include "vb6824.h"
#include <esp_wifi.h>
#include "uart_data_point_manager.h"
#include "settings.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "soft_uart_controller.h"

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
    // Button prev_button_;
    Button next_button_;
    // Minimal idle power-save (no heap, no std::function)
    esp_timer_handle_t idle_timer_ = nullptr;
    volatile int idle_ticks_ = 0;
    static constexpr int SLEEP_SECONDS = 60 * 20;
    static constexpr int SHUTDOWN_SECONDS = -1; // not used


    // 上电计数器相关
    Settings power_counter_settings_;
    esp_timer_handle_t power_counter_timer_;
    static constexpr int POWER_COUNT_THRESHOLD = 5;  // 触发阈值
    static constexpr int POWER_COUNT_RESET_DELAY_MS = 4000;  // 2秒后重置

    int64_t prev_last_click_time_ = 0;
    int64_t next_last_click_time_ = 0;

    // Motion TX controller via soft UART (U0TXD protocol)
    SoftUartController motion_tx_;

    static void IdleTimerCb(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        auto& app = Application::GetInstance();
        if (!app.CanEnterSleepMode()) {
            self->idle_ticks_ = 0;
            return;
        }
        int t = ++self->idle_ticks_;
        if (SLEEP_SECONDS != -1 && t >= SLEEP_SECONDS) {
            ESP_LOGI(TAG, "Idle timeout reached (%d s), entering sleep", t);
            self->run_sleep_mode(true);
        }
        if (SHUTDOWN_SECONDS != -1 && t >= SHUTDOWN_SECONDS) {
            // optional shutdown action
        }
    }

    void InitializePowerSaveTimer() {
        if (idle_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = &CustomBoard::IdleTimerCb,
                .arg = this,
                .name = "idle_timer"
            };
            ESP_ERROR_CHECK(esp_timer_create(&args, &idle_timer_));
        }
        idle_ticks_ = 0;
        ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer_, 1000000));
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

        // next_button_.OnClick([this]() {
        //     int64_t now = esp_timer_get_time();
        //     if (Application::GetInstance().GetDeviceState() == DeviceState::kDeviceStateIdle) {
        //         Application::GetInstance().CancelPlayMusic();
        //         Application::GetInstance().ToggleChatState();
        //         vTaskDelay(pdMS_TO_TICKS(2000));
        //         Application::GetInstance().SendTextToAI("给我播放一首歌");
        //     } else {
        //         if (now - prev_last_click_time_ > 10* 1000000) { // 5秒
        //             prev_last_click_time_ = now;
        //             Application::GetInstance().SendTextToAI("给我播放一首歌");
        //         }
        //     }
        // });
        // volume_up_button_.OnClick([this]() {
        //     auto codec = GetAudioCodec();
        //     auto volume = codec->output_volume() + 10;
        //     if (volume > 100) {
        //         volume = 100;
        //     }
        //     codec->SetOutputVolume(volume);
        // });
        // volume_up_button_.OnLongPress([this]() {
        //     GetAudioCodec()->SetOutputVolume(100);
        // });

        // volume_down_button_.OnClick([this]() {
        //     auto codec = GetAudioCodec();
        //     auto volume = codec->output_volume() - 10;
        //     if (volume < 0) {
        //         volume = 0;
        //     }
        //     codec->SetOutputVolume(volume);
        // });

    }
    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

    void InitializeDataPointManager() {
        // 设置 DataPointManager 的回调函数
        UartDataPointManager::GetInstance().SetCallbacks(
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
            [this](int value) { SetSpeed(value); },
            [this](const uint8_t* data, size_t len) { 
                // 解析协议格式：【动作 A】【动作 B】【时间】
                // 每组 3 字节：动作A(1字节) + 动作B(1字节) + 时间(1字节，单位可能是10ms或100ms，暂时按100ms处理)
                // 时间为 0 时忽略该动作
                const size_t ACTION_GROUP_SIZE = 3; // 每组 3 字节
                size_t group_count = len / ACTION_GROUP_SIZE;
                
                for (size_t i = 0; i < group_count; i++) {
                    size_t offset = i * ACTION_GROUP_SIZE;
                    uint8_t action_a = data[offset];
                    uint8_t action_b = data[offset + 1];
                    uint8_t time_byte = data[offset + 2];
                    // 时间转换为毫秒（单位为秒，最大 255 秒 = 255000ms）
                    uint32_t duration_ms = time_byte * 1000;
                    
                    // 当时间为 0 时忽略这个动作
                    if (time_byte == 0) {
                        ESP_LOGD(TAG, "Group %zu: time is 0, skipping", i);
                        continue;
                    }
                    
                    // 同时发送动作 A 和动作 B（协议：data0 = action_a, data1 = action_b）
                    // 两个动作会同时执行，而不是顺序执行
                    ESP_LOGI(TAG, "Queueing dual motion: A=0x%02X B=0x%02X, duration: %lu ms", 
                             action_a, action_b, (unsigned long)duration_ms);
                    motion_tx_.startMotion(action_a, action_b, duration_ms);
                }
            }
        );
    }


    void InitializePowerManager() {
        PowerManager::GetInstance();
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

    bool isAIWorking() {
        return gpio_get_level(GPIO_NUM_2) == 0;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    next_button_(NEXT_BUTTON_GPIO),
    power_counter_settings_("power_counter", true) {      

        // 初始化上电计数器定时器
        esp_timer_create_args_t timer_args = {
            .callback = &CustomBoard::PowerCounterTimerCallback,
            .arg = this,
            .name = "power_counter_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_counter_timer_));

        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            InitializeIot();
            InitializeDataPointManager();
            InitializePowerSaveTimer();
            // 检查上电计数
            CheckPowerCount();
        }

        InitializePowerManager();

        InitializeGpio(POWER_GPIO, true);

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(LED_GPIO, 0);
        
        audio_codec.OnWakeUp([this](const std::string& command) {
            if (!isAIWorking()) {
                return;
            }
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (command == "你好小智" || command.find("小云") != std::string::npos){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (command == "开始配网") {
                ResetWifiConfiguration();
            }
        });

        // Initialize controller on GPIO1 (TX) and GPIO2 (RX), 19200 8N1 per protocol
        if (motion_tx_.begin(1, 2) != ESP_OK) {
            ESP_LOGE(TAG, "SoftUartController begin failed");
        }
        // 注册U0RXD高低电平回调：高=休眠，低=工作
        motion_tx_.setEnableStateCallback([](bool working, void*){
            if (working) {
                ESP_LOGI(TAG, "U0RXD=LOW -> AI模块工作");
                Application::GetInstance().Schedule([]() {
                    auto& wifi_station = WifiStation::GetInstance();
                    // 工作模式：关闭省电，保证低时延/更稳定吞吐
                    wifi_station.SetPowerSaveMode(false);
                    Application::GetInstance().ToggleChatState();
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
                });
            } else {
                ESP_LOGI(TAG, "U0RXD=HIGH -> AI模块休眠");
                Application::GetInstance().Schedule([]() {
                    auto& wifi_station = WifiStation::GetInstance();
                    // 休眠模式：开启省电，降低功耗（保留连接）
                    Application::GetInstance().QuitTalking();
                    wifi_station.SetPowerSaveMode(true);
                });
            }
        }, nullptr);
    }

    ~CustomBoard() {
        motion_tx_.end();
        if (power_counter_timer_) {
            esp_timer_delete(power_counter_timer_);
        }
        if (idle_timer_) {
            esp_timer_stop(idle_timer_);
            esp_timer_delete(idle_timer_);
            idle_timer_ = nullptr;
        }
    }

    virtual bool ForceSilentStartup() override {
        // gpio2 高电平 则静默启动
        return !isAIWorking();
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


    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    virtual bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return UartDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return UartDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return UartDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return UartDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        UartDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        UartDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

    void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) override {
        UartDataPointManager::GetInstance().ProcessBinaryDataPointValue(name, data, data_len);
    }
};

DECLARE_BOARD(CustomBoard);
