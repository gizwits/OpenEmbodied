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
#include "data_point_manager.h"

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include <cmath>
#include "power_manager.h"
#include "rgb_led.h"
#include "motor_control.h"
#include "esp_adc/adc_oneshot.h"
#include <iot_button.h>

#define TAG "CustomBoard"

// RGB灯光亮度宏定义
#define RGB_LED_BRIGHTNESS 1

#define RESET_WIFI_CONFIGURATION_COUNT 3
#define SLEEP_TIME_SEC 60 * 3

// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;  // BOOT按键（GPIO8）
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    // PowerManager* power_manager_;
    
    // RGB LED 和电机控制实例
    RgbLed rgb_led_;
    MotorControl motor_control_;
    bool motor_on_ = false;
    
    // ADC按钮（使用ESP-IDF库）
    AdcButton* adc_button_k50_;
    AdcButton* adc_button_k51_;
    
    // ADC检测相关（仅用于电池检测）
    adc_oneshot_unit_handle_t adc1_handle_ = nullptr;
    // 防交叉触发节流
    int64_t last_k50_click_ms_ = 0;
    int64_t last_k51_click_ms_ = 0;
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};
    
    // RGB灯光状态管理
    bool rgb_light_on_ = false;
    uint8_t current_color_index_ = 0;
    TaskHandle_t rgb_task_handle_ = nullptr;
    static const uint8_t RGB_COLORS[8][3];
    static constexpr uint8_t RGB_COLOR_COUNT = 8;
    
    // K51按键颜色循环状态
    uint8_t k51_color_mode_ = 7; // 0=全彩渐变, 1=白, 2=红, 3=绿, 4=蓝, 5=黄, 6=青, 7=紫
    
    // 设备电源状态管理
    bool need_power_off_ = false;
    bool device_powered_on_ = true;  // 设备是否开机
    int64_t power_on_time_ = 0;  // 记录上电时间
    
    // BOOT按键三击计数逻辑
    uint8_t boot_button_click_count_ = 0;
    int64_t boot_button_last_click_ms_ = 0;
    

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

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();

        // 检查不在充电就真休眠
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }
    
    // 设备关机方法
    virtual void PowerOff() override {
        ESP_LOGI(TAG, "PowerOff called");
        // 关闭所有功能
        StopRgbLightEffect();
        motor_control_.Stop();
        motor_on_ = false;
        device_powered_on_ = false;
        
        // 关机前播报休眠提示音
        {
            auto codec = GetAudioCodec();
            if (codec) {
                codec->EnableOutput(true);
            }
            Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        
        // 进入深度睡眠
        run_sleep_mode(false);
    }
    void InitializeAdcButtons() {
        ESP_LOGI(TAG, "初始化ADC按钮...");
        
        // 初始化ADC单元
        adc_oneshot_unit_init_cfg_t init_config1 = {
            .unit_id = ADC_UNIT_1,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle_));
        
        // 配置K50按钮 (0V附近)
        button_adc_config_t adc_cfg = {};
        adc_cfg.adc_channel = (adc_channel_t)KEY_ADC_CHANNEL;
        adc_cfg.adc_handle = &adc1_handle_;
        adc_cfg.button_index = 0;
        adc_cfg.min = 0;      // 0V附近
        adc_cfg.max = 200;    // 0.16V以下 (200/4095*3.3V)
        adc_button_k50_ = new AdcButton(adc_cfg);
        
        // 配置K51按钮 (1.65V附近) - 扩大检测范围提高灵敏度
        adc_cfg.button_index = 1;
        adc_cfg.min = 1500;   // 1.21V附近 (1500/4095*3.3V) - 扩大下限
        adc_cfg.max = 2500;   // 2.01V附近 (2500/4095*3.3V) - 扩大上限
        adc_button_k51_ = new AdcButton(adc_cfg);
        
        // 设置K50按钮回调 - 循环切换颜色模式
        adc_button_k50_->OnClick([this]() {
            ESP_LOGI(TAG, " ===== K50按键点击 =====");
            ESP_LOGI(TAG, " 按键类型: K50按键 (ADC检测)");
            ESP_LOGI(TAG, " 检测范围: 0-200 (0V-0.16V)");
            
            if (!device_powered_on_) {
                // 设备关机状态,不响应
                ESP_LOGI(TAG, "设备关机状态,K50按键无效");
                return;
            }

            // 防止刚触发K51后短时间内误触发K50（保持简单的时间互斥,不做ADC硬校验）
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_k51_click_ms_ < 250) {
                ESP_LOGI(TAG, "K50按下但与K51间隔过短,忽略");
                return;
            }
            last_k50_click_ms_ = now_ms;
            
            // 设备开机状态,切换颜色模式
            
            // 停止之前的灯光效果
            if (rgb_light_on_) {
                StopRgbLightEffect();
            }
            
            // 只有电机没有启动时才启动
            if (!motor_on_) {
                motor_control_.SetSpeed(100);
                motor_control_.Start();
                motor_on_ = true;
                ESP_LOGI(TAG, "🔧 电机已启动");
            }
            
            // 先切换到下一个颜色模式
            k51_color_mode_ = (k51_color_mode_ + 1) % 8;
            
            // 根据新的模式显示不同效果
            switch (k51_color_mode_) {
                case 0: // 全彩渐变
                    ESP_LOGI(TAG, "🌈 模式0: 全彩渐变");
                    StartRgbLightEffect();
                    break;
                case 1: // 白色
                    ESP_LOGI(TAG, "🌈 模式1: 白色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(255, 255, 255);
                    break;
                case 2: // 红色
                    ESP_LOGI(TAG, "🌈 模式2: 红色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(255, 0, 0);
                    break;
                case 3: // 绿色
                    ESP_LOGI(TAG, "🌈 模式3: 绿色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(0, 255, 0);
                    break;
                case 4: // 蓝色
                    ESP_LOGI(TAG, "🌈 模式4: 蓝色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(0, 0, 255);
                    break;
                case 5: // 黄色
                    ESP_LOGI(TAG, "🌈 模式5: 黄色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(255, 255, 0);
                    break;
                case 6: // 青色
                    ESP_LOGI(TAG, "🌈 模式6: 青色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(0, 255, 255);
                    break;
                case 7: // 紫色
                    ESP_LOGI(TAG, "🌈 模式7: 紫色");
                    StopRgbLightEffect(); // 先停止渐变任务
                    rgb_light_on_ = true;
                    rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS);
                    rgb_led_.SetColor(255, 0, 255);
                    break;
            }
        });
        
        // 设置K50按钮长按回调 - 关闭灯光和电机
        adc_button_k50_->OnLongPress([this]() {
            ESP_LOGI(TAG, " ===== K50按键长按 =====");
            ESP_LOGI(TAG, " 按键类型: K50按键 (ADC检测)");
            ESP_LOGI(TAG, " 检测范围: 0-200 (0V-0.16V)");
            
            // 设备关机状态下不响应
            if (!device_powered_on_) {
                ESP_LOGI(TAG, "设备关机状态,K50长按无效");
                return;
            }
            
            // 设备开机状态,关闭灯光和电机
            if (rgb_light_on_) {
                // 如果灯光已开启,则关闭
                ESP_LOGI(TAG, "🌈 关闭RGB灯光效果");
                StopRgbLightEffect();
            }
            
            // 关闭电机
            if (motor_on_) {
                motor_control_.Stop();
                motor_on_ = false;
                ESP_LOGI(TAG, "🔧 电机已关闭");
            }
            
            // 重置颜色状态,下次按键从模式0开始
            k51_color_mode_ = 7; // 设为7,这样第一次按键时(7+1)%8=0
        });
        
        // 设置K51按钮点击回调 - 打断AI
        adc_button_k51_->OnClick([this]() {
            ESP_LOGI(TAG, " ===== K51按键按下 =====");
            ESP_LOGI(TAG, " 按键类型: K51按键 (ADC检测)");
            ESP_LOGI(TAG, " 检测范围: 1500-2500 (1.21V-2.01V)");
            
            // 设备关机状态下不响应
            if (!device_powered_on_) {
                ESP_LOGI(TAG, "设备关机状态,K51按键无效");
                return;
            }

            // 防止刚触发K50后短时间内误触发K51（保持简单的时间互斥,不做ADC硬校验）
            int64_t now_ms2 = esp_timer_get_time() / 1000;
            if (now_ms2 - last_k50_click_ms_ < 250) {
                ESP_LOGI(TAG, "K51按下但与K50间隔过短,忽略");
                return;
            }
            last_k51_click_ms_ = now_ms2;
            
            // 设备开机状态,打断AI思考和播放
            auto &app = Application::GetInstance();
            app.ToggleChatState();
            ESP_LOGI(TAG, "K51打断已触发,ToggleChatState 调用完成,device_state_当前: [%d]", app.GetDeviceState());
        });
        
        ESP_LOGI(TAG, "ADC按钮初始化完成");
        
        // 启动ADC调试任务
        xTaskCreate([](void* param) {
            CustomBoard* board = static_cast<CustomBoard*>(param);
            while (true) {
                int adc_value;
                if (adc_oneshot_read(board->adc1_handle_, (adc_channel_t)KEY_ADC_CHANNEL, &adc_value) == ESP_OK) {
                    float voltage = (adc_value * 3.3f) / 4095.0f;
                    // ESP_LOGI(TAG, "🔍 ADC调试: 值=%d, 电压=%.3fV", adc_value, voltage);
                }
                vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒打印一次
            }
        }, "adc_debug", 4096, this, 1, nullptr);
    }

    void InitializeButtons() {
        // 初始化BOOT按键（GPIO8）- 参考gizwits-c2-6824.cc的实现
        // BOOT按键长按 - 立即执行开关机（无需等待松开）
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, " ===== BOOT按键长按 - 立即执行开关机 =====");
            ESP_LOGI(TAG, " 按键类型: BOOT按键 (GPIO8)");
            
            if (device_powered_on_) {
                // 设备开机状态,立即关机
                // 计算设备运行时间
                int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
                int64_t uptime_ms = current_time - power_on_time_;
                ESP_LOGI(TAG, "设备运行时间: %lld ms", uptime_ms);
                
                // 首次上电5秒内忽略关机操作
                const int64_t MIN_UPTIME_MS = 5000; // 5秒
                if (uptime_ms < MIN_UPTIME_MS) {
                    ESP_LOGI(TAG, "首次上电5秒内,忽略关机操作");
                    return;
                }
                
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<CustomBoard*>(arg);
                    ESP_LOGI(TAG, "🔌 设备已关机");
                    board->PowerOff();
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            } else {
                // 设备关机状态,立即开机
                xTaskCreate([](void* arg) {
                    // 开机：统一走冷启动，触发 Application::Start() -> StartNetwork()
                    ESP_LOGI(TAG, "🔌 设备开机（冷启动）");
                    esp_restart();
                    vTaskDelete(NULL);
                }, "power_on_task", 4028, this, 10, NULL);
            }
        });
        
        // BOOT按键松开 - 不再执行开关机（逻辑改为长按即时执行）
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, " ===== BOOT按键松开 =====");
        });
        
        // BOOT按键单击累计计数（600ms 窗口内三击进入配网）
        boot_button_.OnClick([this]() {
            int64_t now_ms = esp_timer_get_time() / 1000;
            const int64_t TRIPLE_CLICK_WINDOW_MS = 600;
            if (now_ms - boot_button_last_click_ms_ > TRIPLE_CLICK_WINDOW_MS) {
                boot_button_click_count_ = 0;
            }
            boot_button_last_click_ms_ = now_ms;

            boot_button_click_count_++;
            if (boot_button_click_count_ >= 3) {
                boot_button_click_count_ = 0;
                // 仅设备开机状态下有效
                if (!device_powered_on_) {
                    ESP_LOGI(TAG, "设备关机状态,配网功能无效");
                    return;
                }
                ESP_LOGI(TAG, " ===== BOOT按键3次点击(软件计数) - 进入配网模式 =====");
                ResetWifiConfiguration();
            }
        });

        // BOOT按键短按连按三次 - 进入配网模式（仅在开机状态下有效）
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, " ===== BOOT按键3次点击 - 进入配网模式 =====");
            ESP_LOGI(TAG, " 按键类型: BOOT按键 (GPIO8)");
            
            // 设备关机状态下不响应
            if (!device_powered_on_) {
                ESP_LOGI(TAG, "设备关机状态,配网功能无效");
                return;
            }
            
            ESP_LOGI(TAG, " 短按连按三次 - 进入配网模式");
            ResetWifiConfiguration();
        }, 3);
        
        // 初始化ADC按钮
        InitializeAdcButtons();
        
        // 配置电池检测通道
        adc_oneshot_chan_cfg_t bat_config = {};
        bat_config.atten = ADC_ATTEN_DB_12;
        bat_config.bitwidth = ADC_BITWIDTH_12;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle_, ADC_CHANNEL_3, &bat_config));
        
        // 将ADC句柄设置给PowerManager
        PowerManager::SetAdcHandle(adc1_handle_);
        
        ESP_LOGI(TAG, " ===== 按键配置完成 =====");
        ESP_LOGI(TAG, " BOOT按键: GPIO%d (Button类检测)", BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, " K50按键: GPIO%d (AdcButton库检测, 0-200 ADC值)", KEY_GPIO);
        ESP_LOGI(TAG, " K51按键: GPIO%d (AdcButton库检测, 1500-2500 ADC值)", KEY_GPIO);
        ESP_LOGI(TAG, " ADC通道: 通道%d (按键), 通道3 (电池), 分辨率: 12位", KEY_ADC_CHANNEL);
        ESP_LOGI(TAG, " 检测方式: BOOT按键用Button类, K50/K51按键用AdcButton库");
        ESP_LOGI(TAG, " 按键功能: K50控制RGB灯光和电机, K51打断AI聊天");
        ESP_LOGI(TAG, " 自动检测: AdcButton库自动处理ADC检测和事件触发");
        ESP_LOGI(TAG, " 调试模式: 每秒打印ADC值和电压");
    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化,添加对 AI 可见设备
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

public:
    // Set short_press_time to a small non-zero value to enable multiple-click detection reliably
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO, false, 2000, 80), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO), 
                    adc_button_k50_(nullptr), adc_button_k51_(nullptr) {      
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动,上电时间戳: %lld ms", power_on_time_);
        
        ESP_LOGE(TAG, "CustomBoard ctor 1 start - ERROR level");
        
        // 配置必要的GPIO
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO) | (1ULL << RGB_LED_R_GPIO) | (1ULL << RGB_LED_G_GPIO) | (1ULL << RGB_LED_B_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        
        // 初始化LED状态
        gpio_set_level(BUILTIN_LED_GPIO, 0);  // 运行指示灯初始开启
        gpio_set_level(RGB_LED_R_GPIO, 0);
        gpio_set_level(RGB_LED_G_GPIO, 0);
        gpio_set_level(RGB_LED_B_GPIO, 0);

        ESP_LOGI(TAG, "Power rails init done");

        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();

        ESP_LOGI(TAG, "Initializing Buttons...");

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();

        ESP_LOGI(TAG, "Initializing LED Signal...");
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动,不然有问题
            InitializeButtons();
        }

        ESP_LOGI(TAG, "Initializing Power Manager...");
        InitializePowerManager();
        ESP_LOGI(TAG, "Power Manager initialized.");

        ESP_LOGI(TAG, "Initializing RGB LED and Motor Control...");
        
        rgb_led_.Initialize();
        motor_control_.Initialize();
        ESP_LOGI(TAG, "RGB LED and Motor Control initialized.");

        // 开机时不启动电机和RGB灯,等待按键触发
        ESP_LOGI(TAG, "🌈 开机完成,等待按键触发电机和灯光");

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

        PowerManager::GetInstance().CheckBatteryStatusImmediately();
        ESP_LOGI(TAG, "Immediately check the battery level upon startup: %d", PowerManager::GetInstance().GetBatteryLevel());


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
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    void EnterDeepSleepIfNotCharging() {
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
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

    // RGB LED 控制接口
    void SetRgbColor(uint8_t r, uint8_t g, uint8_t b) {
        rgb_led_.SetColor(r, g, b);
    }
    
    void SetRgbBrightness(uint8_t brightness) {
        rgb_led_.SetBrightness(brightness);
    }
    
    void StartRgbBreathing(uint8_t r = 255, uint8_t g = 0, uint8_t b = 0) {
        rgb_led_.StartBreathing(r, g, b);
    }
    
    void StopRgbBreathing() {
        rgb_led_.StopBreathing();
    }
    
    // 启动RGB灯光效果
    void StartRgbLightEffect() {
        if (rgb_light_on_) {
            return; // 已经在运行
        }
        
        rgb_light_on_ = true;
        current_color_index_ = 0;
        
        // 设置RGB LED亮度
        rgb_led_.SetBrightness(RGB_LED_BRIGHTNESS); // 使用宏定义亮度
        
        // 创建RGB灯光任务
        xTaskCreate([](void* param) {
            CustomBoard* board = static_cast<CustomBoard*>(param);
            
            // 全彩渐变彩虹色效果 - 无限循环
            ESP_LOGI(TAG, "🌈 开始全彩渐变彩虹色效果");
            
            while (board->rgb_light_on_) {
                // 阶段1: 红 → 橙 (R保持255, G从0递增到165, B保持0)
                for (int g = 0; g <= 165; g += 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(255, g, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 阶段2: 橙 → 黄 (R保持255, G从165递增到255, B保持0)
                for (int g = 165; g <= 255; g += 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(255, g, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 阶段3: 黄 → 绿 (G保持255, R从255递减到0, B保持0)
                for (int r = 255; r >= 0; r -= 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(r, 255, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 阶段4: 绿 → 蓝 (B从0递增到255, G保持255, R保持0)
                for (int b = 0; b <= 255; b += 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(0, 255, b);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 阶段5: 蓝 → 靛 (G从255递减到75, B保持255, R保持0)
                for (int g = 255; g >= 75; g -= 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(0, g, 255);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 阶段6: 靛 → 紫 → 红 (R从0递增到255, B保持255, G保持75)
                for (int r = 0; r <= 255; r += 1) {
                    if (!board->rgb_light_on_) break;
                    board->SetRgbColor(r, 75, 255);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                // 循环衔接: 紫 → 红 (G从75递减到0, R保持255, B从255递减到0)
                for (int i = 0; i <= 75; i += 1) {
                    if (!board->rgb_light_on_) break;
                    int g = 75 - i;
                    int b = 255 - i;
                    board->SetRgbColor(255, g, b);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            
            // 关闭灯光
            board->SetRgbColor(0, 0, 0);
            ESP_LOGI(TAG, "🌈 RGB灯光已关闭");
            
            board->rgb_task_handle_ = nullptr;
            vTaskDelete(nullptr);
        }, "rgb_light_task", 4096, this, 1, &rgb_task_handle_);
    }
    
    // 停止RGB灯光效果
    void StopRgbLightEffect() {
        if (!rgb_light_on_) {
            return; // 已经关闭
        }
        
        rgb_light_on_ = false;
        SetRgbColor(0, 0, 0);
        
        // 删除正在运行的RGB任务
        if (rgb_task_handle_ != nullptr) {
            vTaskDelete(rgb_task_handle_);
            rgb_task_handle_ = nullptr;
            ESP_LOGI(TAG, "🌈 RGB渐变任务已删除");
        }
        
        ESP_LOGI(TAG, "🌈 停止RGB灯光效果");
    }
    
    
    
    // 析构函数
    ~CustomBoard() {
        // 停止RGB灯光效果
        StopRgbLightEffect();
        
        if (adc_button_k50_) {
            delete adc_button_k50_;
            adc_button_k50_ = nullptr;
        }
        if (adc_button_k51_) {
            delete adc_button_k51_;
            adc_button_k51_ = nullptr;
        }
        if (adc1_handle_) {
            adc_oneshot_del_unit(adc1_handle_);
            adc1_handle_ = nullptr;
        }
    }
    
    void TurnOffRgbLed() {
        rgb_led_.TurnOff();
    }

    // 电机控制接口
    void StartMotor() {
        motor_control_.Start();
    }
    
    void StopMotor() {
        motor_control_.Stop();
    }
    
    void SetMotorSpeed(uint8_t speed) {
        motor_control_.SetSpeed(speed);
    }
    
    void SetMotorDirection(bool forward) {
        motor_control_.SetDirection(forward);
    }
    
    bool IsMotorRunning() const {
        return motor_control_.IsRunning();
    }
    
    uint8_t GetMotorSpeed() const {
        return motor_control_.GetSpeed();
    }
    
    bool GetMotorDirection() const {
        return motor_control_.GetDirection();
    }


};

// RGB颜色数组定义 (彩虹色,白,红,绿,蓝,黄,青,紫)
const uint8_t CustomBoard::RGB_COLORS[8][3] = {
    {255, 128, 0},    // 彩虹色 (橙色)
    {255, 255, 255},  // 白色
    {255, 0, 0},      // 红色
    {0, 255, 0},      // 绿色
    {0, 0, 255},      // 蓝色
    {255, 255, 0},    // 黄色
    {0, 255, 255},    // 青色
    {255, 0, 255}     // 紫色
};

void* create_board() { 
    ESP_LOGE("CustomBoard", "create_board() called - creating CustomBoard instance");
    return new CustomBoard(); 
}
