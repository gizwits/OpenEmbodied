#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "motor_driver.h"
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
#include "data_point_manager.h"
#include "settings.h"
#include <esp_timer.h>

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>

#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    VbAduioCodec audio_codec;
    MotorDriver motor_driver;

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
    }

    void InitializeMotorDriver() {
        ESP_LOGW(TAG, "InitializeMotorDriver called");
        ESP_LOGW(TAG, "Calling motor_driver.Initialize()...");
        
        if (!motor_driver.Initialize()) {
            ESP_LOGE(TAG, "Failed to initialize motor driver");
        } else {
            ESP_LOGI(TAG, "Motor driver initialized successfully");
            
            // 先测试一次电位器读取
            ESP_LOGI(TAG, "Testing potentiometer read...");
            int adc_raw = motor_driver.ReadPotentiometerRaw();
            float voltage = motor_driver.ReadPotentiometerVoltage();
            ESP_LOGI(TAG, "Initial test - ADC: %d, Voltage: %.3fV", adc_raw, voltage);
            
            // 启动电位器监控任务，每200ms读取一次
            ESP_LOGI(TAG, "Starting potentiometer monitoring...");
            if (motor_driver.StartPotentiometerMonitoring(200)) {
                ESP_LOGI(TAG, "Potentiometer monitoring started");
            } else {
                ESP_LOGW(TAG, "Failed to start potentiometer monitoring");
            }
        }
    }
    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

    void InitializeDataPointManager() {
        // 设置 DataPointManager 的回调函数
        DataPointManager::GetInstance().SetCallbacks(
            [this]() -> bool { return false; }, // IsCharging - toy 版本可能没有充电功能
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
            [this]() -> int { return 100; }, // 固定亮度 100%
            [this](int value) { 
                /* toy 版本可能没有亮度调节 */
                // 只处理 0 和 100
                if (value == 0) {
                    gpio_set_level(EXTRA_LIGHT_GPIO, 0);
                } 
                if (value == 100) {
                    gpio_set_level(EXTRA_LIGHT_GPIO, 1);
                }
            }
        );
    }


    void InitializePowerManager() {
        PowerManager::GetInstance();
    }


public:
    CustomBoard() : audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO),
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
        ESP_LOGI(TAG, "Factory test mode: %d", s_factory_test_mode);
        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            ESP_LOGI(TAG, "Not in factory test mode, initializing components...");
            InitializeButtons();
            ESP_LOGI(TAG, "Calling InitializeMotorDriver...");
            InitializeMotorDriver();
            InitializeIot();
            InitializeDataPointManager();
            InitializePowerSaveTimer();
            // 检查上电计数
            CheckPowerCount();
        } else {
            ESP_LOGI(TAG, "In factory test mode, skipping motor driver initialization");
        }

        InitializePowerManager();

        // 注册关机回调，确保任何重启路径都会先拉高IO9
        // 防止电机驱动翻车
        esp_register_shutdown_handler([](){
            ESP_LOGI(TAG, "Shutdown handler: Set GPIO9 HIGH before restart");
            gpio_config_t io_conf = {};
            io_conf.pin_bit_mask = (1ULL << GPIO_NUM_9);
            io_conf.mode = GPIO_MODE_OUTPUT;
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_conf.intr_type = GPIO_INTR_DISABLE;
            gpio_config(&io_conf);
            gpio_set_level(GPIO_NUM_9, 1);
        });

        InitializeGpio(POWER_GPIO, true);

        // 根据缓存亮度决定是否点亮灯
        int cached_brightness = -1;
        bool extra_light_on = true;  // 未设置过时默认打开
        {
            Settings dp_settings("datapoint", false);
            cached_brightness = dp_settings.GetInt("brightness", -1);
            if (cached_brightness != -1) {
                extra_light_on = cached_brightness > 0;
            }
        }
        InitializeGpio(EXTRA_LIGHT_GPIO, extra_light_on);
        

        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(LED_GPIO, 0);

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (command == "你好小智" || command.find("小云") != std::string::npos){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                Application::GetInstance().WakeWordInvoke("你好小智");

                ESP_LOGI(TAG, "Motor test triggered by voice command");
                // 在任务中执行测试，避免阻塞语音处理
                xTaskCreate([](void* arg) {
                    CustomBoard* board = static_cast<CustomBoard*>(arg);
                    board->TestMotorActions();
                    vTaskDelete(nullptr);
                }, "motor_test", 4096, this, 5, nullptr);
            } else if (command == "开始配网") {
                ResetWifiConfiguration();
            } else if (command == "前进" || command.find("向前") != std::string::npos) {
                ESP_LOGI(TAG, "Move forward triggered by voice command");
                MoveForward(2000, 60);
            } else if (command == "后退" || command.find("向后") != std::string::npos) {
                ESP_LOGI(TAG, "Move backward triggered by voice command");
                MoveBackward(2000, 60);
            } else if (command == "左转" || command.find("向左") != std::string::npos) {
                ESP_LOGI(TAG, "Turn left triggered by voice command");
                TurnLeft(1500, 50);
            } else if (command == "右转" || command.find("向右") != std::string::npos) {
                ESP_LOGI(TAG, "Turn right triggered by voice command");
                TurnRight(1500, 50);
            } else if (command == "停止" || command.find("停下") != std::string::npos) {
                ESP_LOGI(TAG, "Stop triggered by voice command");
                StopMotors();
            }
        });
    }

    ~CustomBoard() {
        if (power_counter_timer_) {
            esp_timer_delete(power_counter_timer_);
        }
        if (idle_timer_) {
            esp_timer_stop(idle_timer_);
            esp_timer_delete(idle_timer_);
            idle_timer_ = nullptr;
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

    // 电机控制相关公共方法
    MotorDriver& GetMotorDriver() {
        return motor_driver;
    }

    // 便捷的电机控制方法
    void MoveForward(int duration_ms = -1, int speed = 100) {
        motor_driver.MoveForward(duration_ms, speed);
    }

    void MoveBackward(int duration_ms = -1, int speed = 100) {
        motor_driver.MoveBackward(duration_ms, speed);
    }

    void TurnLeft(int duration_ms = -1, int speed = 100) {
        motor_driver.TurnLeft(duration_ms, speed);
    }

    void TurnRight(int duration_ms = -1, int speed = 100) {
        motor_driver.TurnRight(duration_ms, speed);
    }

    void StopMotors() {
        motor_driver.Stop();
    }

    void BrakeMotors() {
        motor_driver.Brake();
    }

    // 电机动作测试方法
    void TestMotorActions() {
        ESP_LOGI(TAG, "Starting motor action test sequence...");
        
        // 测试1: 前进2秒
        ESP_LOGI(TAG, "Test 1: Moving forward for 2 seconds");
        MoveForward(2000, 80);
        vTaskDelay(pdMS_TO_TICKS(3000)); // 等待动作完成
        
        // 测试2: 后退2秒
        ESP_LOGI(TAG, "Test 2: Moving backward for 2 seconds");
        MoveBackward(2000, 80);
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        // 测试3: 左转1.5秒
        ESP_LOGI(TAG, "Test 3: Turning left for 1.5 seconds");
        TurnLeft(1500, 70);
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // 测试4: 右转1.5秒
        ESP_LOGI(TAG, "Test 4: Turning right for 1.5 seconds");
        TurnRight(1500, 70);
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // 测试5: 刹车测试
        ESP_LOGI(TAG, "Test 5: Brake test");
        MoveForward(1000, 60); // 先前进1秒
        vTaskDelay(pdMS_TO_TICKS(1200));
        BrakeMotors(); // 然后刹车
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // 测试6: 不同速度测试
        ESP_LOGI(TAG, "Test 6: Speed variation test");
        for (int speed = 30; speed <= 100; speed += 20) {
            ESP_LOGI(TAG, "Testing speed: %d%%", speed);
            MoveForward(1000, speed);
            vTaskDelay(pdMS_TO_TICKS(1200));
            StopMotors();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        // 测试7: 自定义动作测试
        ESP_LOGI(TAG, "Test 7: Custom action test");
        MotorDriver::Action custom_action;
        custom_action.type = MotorDriver::ACTION_CUSTOM;
        custom_action.duration_ms = 2000;
        custom_action.speed_a = 60;  // 电机A 60%速度
        custom_action.speed_b = 90;  // 电机B 90%速度
        custom_action.callback = []() {
            ESP_LOGI(TAG, "Custom action completed!");
        };
        motor_driver.ExecuteCustomAction(custom_action);
        vTaskDelay(pdMS_TO_TICKS(2500));
        
        // 测试8: 停止所有电机
        ESP_LOGI(TAG, "Test 8: Final stop");
        StopMotors();
        
        ESP_LOGI(TAG, "Motor action test sequence completed!");
    }

    // 简单的电机控制测试（用于语音命令等）
    void SimpleMotorTest() {
        ESP_LOGI(TAG, "Simple motor test: forward 1 second");
        MoveForward(1000, 50);
    }
};

DECLARE_BOARD(CustomBoard);
