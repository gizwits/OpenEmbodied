#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "audio_codecs/box_audio_codec.h"
#include "power_manager.h"
#include "assets/lang_config.h"

#include "led/single_led.h"
#include "xunguan_display.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"

#include <math.h>

#define TAG "MovecallMojiESP32S3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

#define LIS2HH12_I2C_ADDR 0x1D  // SDO接GND为0x1D，接VDD为0x1E
#define LIS2HH12_INT1_PIN GPIO_NUM_42

class MovecallMojiESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    XunguanDisplay* display_;
    bool need_power_off_ = false;
    i2c_master_bus_handle_t i2c_bus_;
    // LIS2HH12专用I2C
    i2c_master_bus_handle_t lis2hh12_i2c_bus_;
    i2c_master_dev_handle_t lis2hh12_dev_;
    int64_t power_on_time_ = 0;  // 记录上电时间
    bool is_sleep_ = false;
    PowerManager* power_manager_;


    static void lis2hh12_task(void* arg) {
        MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
        float last_ax = 0, last_ay = 0, last_az = 0;
        const float threshold = 0.5; // g-force
        int shake_count = 0;
        const int shake_count_threshold = 10; // 连续3次才算shake
        const int shake_count_decay = 1;     // 每次没检测到就-1
        while (1) {
            // 读取X/Y/Z
            int16_t x = (int16_t)((board->lis2hh12_read_reg_pub(0x29) << 8) | board->lis2hh12_read_reg_pub(0x28));
            int16_t y = (int16_t)((board->lis2hh12_read_reg_pub(0x2B) << 8) | board->lis2hh12_read_reg_pub(0x2A));
            int16_t z = (int16_t)((board->lis2hh12_read_reg_pub(0x2D) << 8) | board->lis2hh12_read_reg_pub(0x2C));
            float ax = x * 0.061f / 1000.0f;
            float ay = y * 0.061f / 1000.0f;
            float az = z * 0.061f / 1000.0f;
            if (fabs(ax - last_ax) > threshold || fabs(ay - last_ay) > threshold || fabs(az - last_az) > threshold) {
                shake_count++;
                if (shake_count >= shake_count_threshold) {
                    ESP_LOGI("LIS2HH12", "Shake detected! ax=%.2f ay=%.2f az=%.2f", ax, ay, az);
                    shake_count = 0; // 触发后清零
                    // 这里可以触发你的摇晃事件
                    if (board->ChannelIsOpen()) {
                        board->display_->SetEmotion("vertigo");
                        Application::GetInstance().SendMessage("用户正在摇晃你");
                    } else {
                        ESP_LOGI("LIS2HH12", "Channel is not open");
                    }
                }
            } else {
                if (shake_count > 0) shake_count -= shake_count_decay;
            }
            last_ax = ax; last_ay = ay; last_az = az;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
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

    // SPI初始化
    void InitializeSpi() {
        // SPI initialization is now handled by XunguanDisplay
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        // Create and initialize XunguanDisplay
        display_ = new XunguanDisplay();
        if (!display_->Initialize()) {
            ESP_LOGE(TAG, "Failed to initialize XunguanDisplay");
        }
    }

    int MaxBacklightBrightness() {
        return 60;
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

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "first_level: %d", first_level);

        touch_button_.OnPressDown([this]() {
            //切换表情
            ESP_LOGI(TAG, "touch_button_.OnPressDown");

            display_->SetEmotion("loving");
            if (ChannelIsOpen()) {
                Application::GetInstance().SendMessage("用户正在抚摸你");
            } else {
                ESP_LOGI("touch", "Channel is not open");
            }
        });

        boot_button_.OnClick([this]() {

            auto& app = Application::GetInstance();
            // if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
            //     ResetWifiConfiguration();
            // }
            app.ToggleChatState();
            // display_->TestNextEmotion();
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
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
                ESP_LOGI(TAG, "执行关机操作");
                Application::GetInstance().QuitTalking();
                // vTaskDelay(pdMS_TO_TICKS(200));
                // auto codec = GetAudioCodec();
                // codec->EnableOutput(true);
                // Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);

                    if (board->IsCharging()) {
                        // 充电中，只关闭背光
                        board->GetBacklight()->SetBrightness(0, false);
                        board->is_sleep_ = true;
                    } else {
                        // 没有充电，关机
                        gpio_set_level(POWER_GPIO, 0);
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        boot_button_.OnMultipleClick([this]() {
            ResetWifiConfiguration();
        }, 3);
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

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
    }

    // LIS2HH12专用I2C初始化
    void InitializeLis2hh12I2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0, // 用另一个I2C控制器
            .sda_io_num = GPIO_NUM_38,      // LIS2HH12的SDA
            .scl_io_num = GPIO_NUM_41,      // LIS2HH12的SCL
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &lis2hh12_i2c_bus_));
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = LIS2HH12_I2C_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &lis2hh12_dev_));
    }

    void InitializeLis2hh12() {
        // 0x20: CTRL1, 0x57 = 100Hz, all axes enable, normal mode
        this->lis2hh12_write_reg(0x20, 0x57);
        // 0x23: CTRL4, 0x00 = continuous update, LSB at lower address
        this->lis2hh12_write_reg(0x23, 0x00);
    }

    // LIS2HH12 I2C读写成员函数
    uint8_t lis2hh12_read_reg(uint8_t reg) {
        uint8_t data = 0;
        i2c_master_transmit_receive(lis2hh12_dev_, &reg, 1, &data, 1, 100 / portTICK_PERIOD_MS);
        return data;
    }
    void lis2hh12_write_reg(uint8_t reg, uint8_t value) {
        uint8_t buf[2] = {reg, value};
        i2c_master_transmit(lis2hh12_dev_, buf, 2, 100 / portTICK_PERIOD_MS);
    }

    virtual bool NeedPlayProcessVoice() override {
        return true;
    }


    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(GPIO_NUM_NC, GPIO_NUM_NC, BAT_ADC_UNIT, BAT_ADC_CHANNEL);
        

        // 注册充电状态改变回调
        power_manager_->SetChargingStatusCallback([this](bool is_charging) {
            ESP_LOGI(TAG, "充电状态改变: %s", is_charging ? "开始充电" : "停止充电");
            XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
            if (is_charging) {
                // 充电开始时的处理逻辑
                ESP_LOGI(TAG, "检测到开始充电");
                // 降低发热                
                GetBacklight()->SetBrightness(5, false);
                
                // 设置充电时的自定义帧率：100-125Hz (8-10ms延迟)
                // 需要强制转换成 XunguanDisplay 类型
                if (xunguan_display) {
                    if (xunguan_display->SetCustomFrameRate(20, 30)) {
                        ESP_LOGI(TAG, "充电帧率设置成功");
                    } else {
                        ESP_LOGE(TAG, "充电帧率设置失败");
                    }
                } else {
                    ESP_LOGE(TAG, "无法获取 XunguanDisplay 对象");
                }
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");
                
                // 恢复正常帧率模式
                // 需要强制转换成 XunguanDisplay 类型
                if (xunguan_display) {
                    ESP_LOGI(TAG, "停止充电，恢复正常帧率模式");
                    if (xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL)) {
                        ESP_LOGI(TAG, "正常帧率模式恢复成功");
                    } else {
                        ESP_LOGE(TAG, "正常帧率模式恢复失败");
                    }
                } else {
                    ESP_LOGE(TAG, "无法获取 XunguanDisplay 对象");
                }
                
                if (is_sleep_) {
                    // 关机
                    gpio_set_level(POWER_GPIO, 0);
                } else {
                    GetBacklight()->RestoreBrightness();
                }
            }
        });
    }

public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO) { 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);
        
        InitializeChargingGpio();

        bool is_charging = IsCharging();
        // if (is_charging) {
        //     // 充电中
        // } else {
        //     // 没有充电
        //     InitializeGpio(POWER_GPIO, true);
        // }
        InitializeGpio(POWER_GPIO, true);

        ESP_LOGI(TAG, "is_charging: %d", is_charging);
        InitializeI2c();
        InitializeGpio(AUDIO_CODEC_PA_PIN, true);
        // InitializeGpio(DISPLAY_BACKLIGHT_PIN, false);
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeLis2hh12I2c(); // 新增LIS2HH12专用I2C
        InitializeLis2hh12();    // 初始化LIS2HH12
        InitializeButtons();
        InitializeIot();
        xTaskCreate(MovecallMojiESP32S3::lis2hh12_task, "lis2hh12_task", 4096, this, 5, NULL); // 启动检测任务
        InitializePowerManager();
        // ESP_LOGI(TAG, "ReadADC2_CH1_Oneshot");
        // ReadADC2_CH1_Oneshot();
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
        }
        xTaskCreate(
            RestoreBacklightTask,      // 任务函数
            "restore_backlight",       // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );
    }

    static void RestoreBacklightTask(void* arg) {
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        int level;
        bool charging, discharging;
        self->GetBatteryLevel(level, charging, discharging);
        XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(self->GetDisplay());
        self->GetBacklight()->RestoreBrightness();

        if (charging) {
            // 降低发热            
            xunguan_display->SetCustomFrameRate(20, 30);
        } else {
            xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);
        }
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
        int chrg = gpio_get_level(CHARGING_PIN);
        int standby = gpio_get_level(STANDBY_PIN);
        ESP_LOGI(TAG, "chrg: %d, standby: %d", chrg, standby);
        return chrg == 0 || standby == 0;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
        return true;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    // 公开I2C读寄存器方法供任务调用
    uint8_t lis2hh12_read_reg_pub(uint8_t reg) { return this->lis2hh12_read_reg(reg); }
};

DECLARE_BOARD(MovecallMojiESP32S3);