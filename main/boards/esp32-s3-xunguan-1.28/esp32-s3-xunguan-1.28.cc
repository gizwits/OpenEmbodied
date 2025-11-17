#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "audio/codecs/box_audio_codec.h"
#include "power_manager.h"
#include "assets/lang_config.h"
#include "font_awesome_symbols.h"
#include "wifi_connection_manager.h"
#include "device_state_event.h"


#include "led/single_led.h"
// #include "xunguan_display.h"
#include "display/eye_display.h"
#include "display/display.h"
#include "display/video_player.h"

#include "w25q64_flash.h"

#include <wifi_station.h>
#include "power_save_timer.h"
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_partition.h>
#include <functional>
#include <freertos/timers.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"

#include <math.h>

#define TAG "MovecallMojiESP32S3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

#define LIS2HH12_I2C_ADDR 0x1D  // SDO接GND为0x1D，接VDD为0x1E
#define LIS2HH12_INT1_PIN GPIO_NUM_42

// 播放模式枚举
enum class PlaybackMode {
    DISPLAY_ANIMATION,  // Display动画模式（默认）
    VIDEO_PLAYBACK      // 视频播放模式
};

// 前向声明
class MovecallMojiESP32S3;

// Display包装类：拦截SetEmotion调用并路由到Board的TriggerEmotion
class DisplayWrapper : public Display {
private:
    Display* wrapped_display_;
    MovecallMojiESP32S3* board_;

public:
    DisplayWrapper(Display* display, MovecallMojiESP32S3* board) 
        : wrapped_display_(display), board_(board) {
        // 复制基本属性
        width_ = display->width();
        height_ = display->height();
    }

    // 实现纯虚函数
    // 直接使用底层 LVGL 锁定机制，与 EyeDisplay 保持一致
    virtual bool Lock(int timeout_ms = 0) override {
        if (wrapped_display_) {
            // 由于 DisplayWrapper 是 Display 的 friend，可以直接调用 protected 方法
            // 但为了安全，我们使用底层锁定机制
            return lvgl_port_lock(timeout_ms);
        }
        return true;
    }

    virtual void Unlock() override {
        if (wrapped_display_) {
            // 使用底层解锁机制
            lvgl_port_unlock();
        }
    }

    virtual void SetEmotion(const char* emotion) override;

    // 转发其他方法到wrapped_display_
    virtual void SetStatus(const char* status) override {
        if (wrapped_display_) wrapped_display_->SetStatus(status);
    }
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        if (wrapped_display_) wrapped_display_->ShowNotification(notification, duration_ms);
    }
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override {
        if (wrapped_display_) wrapped_display_->ShowNotification(notification, duration_ms);
    }
    virtual void SetChatMessage(const char* role, const char* content) override {
        if (wrapped_display_) wrapped_display_->SetChatMessage(role, content);
    }
    virtual void SetIcon(const char* icon) override {
        if (wrapped_display_) wrapped_display_->SetIcon(icon);
    }
    virtual void SetPreviewImage(const lv_img_dsc_t* image) override {
        if (wrapped_display_) wrapped_display_->SetPreviewImage(image);
    }
    virtual void SetTheme(const std::string& theme_name) override {
        if (wrapped_display_) wrapped_display_->SetTheme(theme_name);
    }
    virtual void UpdateStatusBar(bool update_all = false) override {
        if (wrapped_display_) wrapped_display_->UpdateStatusBar(update_all);
    }
    virtual void EnterWifiConfig() override {
        if (wrapped_display_) wrapped_display_->EnterWifiConfig();
    }
    virtual void EnterOTAMode() override {
        if (wrapped_display_) wrapped_display_->EnterOTAMode();
    }
    virtual void ClearScreen() override {
        if (wrapped_display_) wrapped_display_->ClearScreen();
    }
    virtual void SetOTAProgress(int progress) override {
        if (wrapped_display_) wrapped_display_->SetOTAProgress(progress);
    }
    virtual void EnterTestMode() override {
        if (wrapped_display_) wrapped_display_->EnterTestMode();
    }
    virtual void SetTestItems(const std::vector<TestItem>& test_items) override {
        if (wrapped_display_) wrapped_display_->SetTestItems(test_items);
    }
    virtual void UpdateTestItem(const std::string& id, bool pass) override {
        if (wrapped_display_) wrapped_display_->UpdateTestItem(id, pass);
    }
    virtual void UpdateTestItemStatus(const std::string& id, int status) override {
        if (wrapped_display_) wrapped_display_->UpdateTestItemStatus(id, status);
    }
    virtual void StartRGBTest() override {
        if (wrapped_display_) wrapped_display_->StartRGBTest();
    }
    virtual void StopRGBTest() override {
        if (wrapped_display_) wrapped_display_->StopRGBTest();
    }
};

class MovecallMojiESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    EyeDisplay* display_;
    DisplayWrapper* display_wrapper_ = nullptr;  // Display包装器
    // LCD handles for direct frame blitting
    esp_lcd_panel_io_handle_t panel_io_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    bool need_power_off_ = false;
    i2c_master_bus_handle_t i2c_bus_;
    // LIS2HH12专用I2C
    i2c_master_bus_handle_t lis2hh12_i2c_bus_;
    i2c_master_dev_handle_t lis2hh12_dev_;
    int64_t power_on_time_ = 0;  // 记录上电时间
    PowerManager* power_manager_;
    TickType_t last_touch_time_ = 0;  // 上次抚摸触发时间
    PowerSaveTimer* power_save_timer_;
    bool is_charging_sleep_ = false;

    // 视频播放器（使用封装的 VideoPlayer 类）
    VideoPlayer* video_player_ = nullptr;
    TickType_t allow_switch_after_tick_ = 0;
    
    // 播放模式：默认使用Display动画模式
    PlaybackMode playback_mode_ = PlaybackMode::DISPLAY_ANIMATION;
    
    // 电量显示时保存的视频组索引（用于恢复视频播放）
    int saved_video_group_index_for_battery_ = -1;
    
    // 双击检测相关变量
    uint8_t boot_button_click_count_ = 0;
    int64_t boot_button_last_click_ms_ = 0;
    esp_timer_handle_t boot_button_timer_ = nullptr;
    static constexpr uint32_t kDoubleClickWindowMs = 800;  // 双击检测窗口：800ms（增加时间窗口，提高双击检测成功率）
    
    // 快速四击检测相关变量
    uint8_t quick_click_count_ = 0;
    int64_t quick_click_first_time_ms_ = 0;
    static constexpr uint32_t kQuickClickWindowMs = 450;  // 快速四击检测窗口：400ms
    
    
    // 二维码显示状态
    bool qrcode_displaying_ = false;  // 是否正在显示二维码
    esp_timer_handle_t qrcode_timer_ = nullptr;  // 二维码显示后的定时器（用于自动进入配网模式）

    std::vector<TestItem> test_items = {
        {"lcd", "LCD测试", 1},
        {"key", "按键测试", 0},
        {"wifi", "WiFi连接测试", 0},
        {"sensor", "陀螺仪测试", 0},
        {"battery", "电池检测", 0},
        {"mic", "麦克风检测", 0},
    };


    void InitializePowerSaveTimer() {
        // 20 分钟进休眠
        // 30 分钟 关机
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 20, 60 * 30);
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


    static void lis2hh12_task(void* arg) {
        MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
        float last_ax = 0, last_ay = 0, last_az = 0;
        const float threshold = 0.5; // g-force
        int shake_count = 0;
        const int shake_count_threshold = 10; // 连续3次才算shake
        const int shake_count_decay = 1;     // 每次没检测到就-1
        TickType_t last_shake_time = 0;      // 上次摇晃触发时间
        const TickType_t shake_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间
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
                    TickType_t current_time = xTaskGetTickCount();
                    // 检查是否已经过了冷却时间
                    if (current_time - last_shake_time >= shake_cooldown) {
                        ESP_LOGI("LIS2HH12", "Shake detected! ax=%.2f ay=%.2f az=%.2f", ax, ay, az);
                        last_shake_time = current_time; // 更新上次触发时间
                        shake_count = 0; // 触发后清零

                        // 根据当前模式触发表情
                        if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                            // 视频模式：只切换视频组，不触发AI对话
                            board->CycleVideoGroup();
                        } else {
                            // Display模式：显示表情并触发AI对话
                            board->TriggerEmotion("vertigo");
                            
                            if (Application::GetInstance().IsTmpFactoryTestMode()) {
                                board->display_->UpdateTestItem("sensor", 1);
                            } else {
                                // 这里可以触发你的摇晃事件
                                if (board->ChannelIsOpen()) {
                                    #ifdef CONFIG_LANGUAGE_ZH_CN
                                    Application::GetInstance().SendTextToAI("用户正在摇晃你");
                                    #else
                                    Application::GetInstance().SendTextToAI("User is shaking you");
                                    #endif
                                } else {
                                    ESP_LOGI("LIS2HH12", "Channel is not open");
                                }
                            }
                        }

                    } else {
                        ESP_LOGI("LIS2HH12", "Shake detected but in cooldown period");
                        shake_count = 0; // 重置计数但不触发
                    }
                }
            } else {
                if (shake_count > 0) shake_count -= shake_count_decay;
            }
            last_ax = ax; last_ay = ay; last_az = az;
            vTaskDelay(pdMS_TO_TICKS(100));
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

    // GC9A01初始化
    void InitializeGc9a01Display() {
        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = DISPLAY_SPI_CS_PIN,
            .dc_gpio_num = DISPLAY_SPI_DC_PIN,
            .spi_mode = 0,
            .pclk_hz = DISPLAY_SPI_SCLK_HZ,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        
        esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel IO creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
            .bits_per_pixel = 16,
        };
        
        ret = esp_lcd_new_panel_gc9a01(panel_io_handle_, &panel_config, &panel_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_reset(panel_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_init(panel_handle_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Invert colors for GC9A01
        ret = esp_lcd_panel_invert_color(panel_handle_, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel color invert failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Mirror display
        ret = esp_lcd_panel_mirror(panel_handle_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel mirror failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Turn on display
        ret = esp_lcd_panel_disp_on_off(panel_handle_, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel display on failed: %s", esp_err_to_name(ret));
            return;
        }
        
        display_ = new EyeDisplay(panel_io_handle_, panel_handle_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            &qrcode_img,
            {
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),
            });
        
        // 创建视频播放器实例
        video_player_ = new VideoPlayer(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

    // 重写 Board 基类的 PlayVideoGroup 方法，使用 VideoPlayer 类（和 SetEmotion 一样的调用方式）
    void PlayVideoGroup(const char* emotion) override {
        if (emotion == nullptr || video_player_ == nullptr) {
            ESP_LOGE(TAG, "PlayVideoGroup: emotion is nullptr or video_player_ is nullptr");
            return;
        }
        // 使用 VideoPlayer 类播放视频
        video_player_->PlayVideoGroup(emotion);
        ESP_LOGI(TAG, "PlayVideoGroup: emotion=%s", emotion);
    }

public:
    // 统一的表情触发函数：根据当前模式选择使用display动画或视频播放
    void TriggerEmotion(const char* emotion) {
        if (emotion == nullptr) {
            return;
        }
        
        if (playback_mode_ == PlaybackMode::DISPLAY_ANIMATION) {
            // Display动画模式：使用EyeDisplay的SetEmotion
            if (display_ != nullptr) {
                display_->SetEmotion(emotion);
                ESP_LOGI(TAG, "TriggerEmotion (Display): %s", emotion);
            }
            // 确保视频播放停止并隐藏视频图像
            if (video_player_ != nullptr) {
                video_player_->StopPlayback();
            }
        } else {
            // 视频播放模式：使用VideoPlayer播放
            if (video_player_ != nullptr) {
                PlayVideoGroup(emotion);
                ESP_LOGI(TAG, "TriggerEmotion (Video): %s", emotion);
            }
            // 确保眼睛动画隐藏（EyeDisplay在视频模式下会自动隐藏眼睛）
            // 视频播放时，VideoPlayer会显示在最前面，覆盖眼睛动画
        }
    }

    // 切换播放模式
    void SwitchPlaybackMode() {
        auto& app = Application::GetInstance();
        
        if (playback_mode_ == PlaybackMode::DISPLAY_ANIMATION) {
            playback_mode_ = PlaybackMode::VIDEO_PLAYBACK;
            ESP_LOGI(TAG, "切换到视频播放模式");
            
            // 先停止AI对话和音乐播放
            app.QuitTalking();  // 停止AI对话，关闭音频通道
            app.CancelPlayMusic();  // 停止音乐播放
            ESP_LOGI(TAG, "已停止AI对话和音乐播放");
            
            // 停止display动画，隐藏所有Display对象（包括zzz等递归隐藏）
            // 注意：充电圆环不会被隐藏，因为它是在屏幕级别创建的独立对象
            if (display_ != nullptr) {
                // 使用Lock/Unlock来保护LVGL操作
                if (display_->Lock(1000)) {
                    // 先删除zzz对象，避免显示在视频上（在锁内删除确保线程安全）
                    display_->DeleteZzzObjects();
                    
                    lv_obj_t* screen = lv_screen_active();
                    if (screen != nullptr) {
                        // 获取充电圆环对象指针，在隐藏时排除它
                        EyeDisplay* eye_display = static_cast<EyeDisplay*>(display_);
                        lv_obj_t* charging_arc = (eye_display != nullptr) ? eye_display->GetChargingBatteryArc() : nullptr;
                        
                        // 递归函数：隐藏对象及其所有子对象（包括zzz）
                        // 注意：排除充电圆环，确保它一直显示
                        std::function<void(lv_obj_t*)> add_hidden_recursive;
                        add_hidden_recursive = [&add_hidden_recursive, charging_arc](lv_obj_t* obj) -> void {
                            if (obj == nullptr) return;
                            // 如果是充电圆环，跳过隐藏
                            if (obj == charging_arc) {
                                ESP_LOGI("MovecallMojiESP32S3", "跳过隐藏充电圆环");
                                return;
                            }
                            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
                            for (uint32_t i = 0; i < child_cnt; i++) {
                                lv_obj_t* child = lv_obj_get_child(obj, i);
                                if (child != nullptr) {
                                    add_hidden_recursive(child);
                                }
                            }
                        };
                        
                        // 隐藏屏幕的所有子对象（递归隐藏，包括zzz）
                        // 注意：排除充电圆环，确保它一直显示
                        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t* child = lv_obj_get_child(screen, i);
                            if (child != nullptr && child != charging_arc) {  // 排除充电圆环
                                add_hidden_recursive(child);
                            } else if (child == charging_arc) {
                                ESP_LOGI("MovecallMojiESP32S3", "跳过隐藏充电圆环（顶层检查）");
                            }
                        }
                        
                        // 如果充电圆环存在，确保它在最前面且可见
                        if (charging_arc != nullptr && lv_obj_is_valid(charging_arc)) {
                            lv_obj_move_foreground(charging_arc);
                            lv_obj_clear_flag(charging_arc, LV_OBJ_FLAG_HIDDEN);
                            ESP_LOGI("MovecallMojiESP32S3", "确保充电圆环在最前面且可见");
                        }
                    }
                    display_->Unlock();
                }
            }
            
            // 检查充电状态，如果正在充电，显示电量圆环（因为状态改变回调可能不会触发）
            // 在隐藏Display对象之后检查，确保圆环显示在最前面
            if (IsCharging()) {
                ESP_LOGI(TAG, "切换到视频模式时检测到正在充电，显示电量圆环");
                auto device_state = app.GetDeviceState();
                if (device_state != kDeviceStateWifiConfiguring && display_ != nullptr) {
                    int battery_level = 0;
                    bool charging = false;
                    bool discharging = false;
                    if (GetBatteryLevel(battery_level, charging, discharging)) {
                        display_->ShowBatteryIndicatorForCharging(battery_level);
                    } else {
                        display_->ShowBatteryIndicatorForCharging();
                    }
                }
            }
            
            // 启动视频播放
            if (video_player_ != nullptr) {
                video_player_->PlayVideoGroup("neutral");
            }
            
            // 视频模式下禁用唤醒词检测和语音处理（离线模式）
            app.GetAudioService().EnableWakeWordDetection(false);
            app.GetAudioService().EnableVoiceProcessing(false);
            ESP_LOGI(TAG, "视频模式：已禁用唤醒词检测和语音处理");
        } else {
            playback_mode_ = PlaybackMode::DISPLAY_ANIMATION;
            ESP_LOGI(TAG, "切换到Display动画模式");
            
            // Display模式下恢复唤醒词检测（根据设备状态）
            auto device_state = app.GetDeviceState();
            ESP_LOGI(TAG, "切换回Display模式，当前设备状态: %d", device_state);
            
            // 延迟一下，确保所有状态更新完成后再恢复唤醒词检测
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // 重新获取设备状态（可能已经改变）
            device_state = app.GetDeviceState();
            ESP_LOGI(TAG, "切换回Display模式，延迟后设备状态: %d", device_state);
            
            // 如果设备状态是idle或sleeping，恢复唤醒词检测
            if (device_state == kDeviceStateIdle || device_state == kDeviceStateSleeping) {
                app.GetAudioService().EnableWakeWordDetection(true);
                app.GetAudioService().EnableVoiceProcessing(false);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（idle/sleeping状态）");
            } else if (device_state == kDeviceStateListening || device_state == kDeviceStateSpeaking) {
                // 在listening或speaking状态下，也需要启用唤醒词检测（可以在speaking时打断）
                #if CONFIG_USE_AFE_WAKE_WORD
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（listening/speaking状态，AFE唤醒词）");
                #else
                // 非AFE唤醒词，在speaking状态下不启用唤醒词检测
                if (device_state == kDeviceStateListening) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                    ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（listening状态）");
                } else {
                    ESP_LOGW(TAG, "Display模式：speaking状态下非AFE唤醒词不启用唤醒词检测");
                }
                #endif
            } else {
                // 其他状态，也尝试启用唤醒词检测（如果设备允许）
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（其他状态）");
            }
            // 停止视频播放，显示display动画
            if (video_player_ != nullptr) {
                video_player_->StopPlayback();
                // 等待一下确保视频任务完全退出
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (display_ != nullptr) {
                ESP_LOGI(TAG, "Switching back to Display mode: clearing hidden flags and resetting emotion");
                // 先停止视频播放并隐藏视频图像
                if (video_player_ != nullptr) {
                    video_player_->StopPlayback();
                    // 额外确保视频图像被隐藏（在Display锁内操作）
                    if (display_->Lock(1000)) {
                        // 查找并隐藏视频图像对象
                        lv_obj_t* screen = lv_screen_active();
                        if (screen != nullptr) {
                            uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                            for (uint32_t i = 0; i < child_cnt; i++) {
                                lv_obj_t* child = lv_obj_get_child(screen, i);
                                if (child != nullptr && lv_obj_check_type(child, &lv_image_class)) {
                                    // 找到图像对象，可能是视频图像
                                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_move_background(child);
                                    ESP_LOGI(TAG, "Hidden and moved background for potential video image object");
                                }
                            }
                        }
                        display_->Unlock();
                    }
                    // 等待一下确保视频任务完全退出
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                // 显示Display对象（取消隐藏）
                if (display_->Lock(1000)) {
                    lv_obj_t* screen = lv_screen_active();
                    if (screen != nullptr) {
                        // 递归函数：取消隐藏对象及其所有子对象
                        std::function<void(lv_obj_t*)> clear_hidden_recursive;
                        clear_hidden_recursive = [&clear_hidden_recursive](lv_obj_t* obj) -> void {
                            if (obj == nullptr) return;
                            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
                            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
                            for (uint32_t i = 0; i < child_cnt; i++) {
                                lv_obj_t* child = lv_obj_get_child(obj, i);
                                if (child != nullptr) {
                                    clear_hidden_recursive(child);
                                }
                            }
                        };
                        
                        // 显示屏幕的所有子对象（递归取消隐藏）
                        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                        ESP_LOGI(TAG, "Screen has %u children, clearing HIDDEN flags and moving to foreground", child_cnt);
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t* child = lv_obj_get_child(screen, i);
                            if (child != nullptr) {
                                // 跳过视频图像对象（图像类型）
                                if (lv_obj_check_type(child, &lv_image_class)) {
                                    ESP_LOGI(TAG, "Skipping image object (likely video image)");
                                    continue;
                                }
                                clear_hidden_recursive(child);
                                // 确保Display对象显示在最前面
                                lv_obj_move_foreground(child);
                                ESP_LOGI(TAG, "Moved child %u to foreground", i);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "Screen is nullptr");
                    }
                    display_->Unlock();
                } else {
                    ESP_LOGW(TAG, "Failed to lock display");
                }
                // 立即设置表情，确保Display动画显示
                ESP_LOGI(TAG, "Setting emotion to neutral after switching back from video mode");
                display_->SetEmotion("neutral");
            } else {
                ESP_LOGW(TAG, "Display is nullptr");
            }
        }
    }

    // 循环切换Display表情
    void CycleDisplayEmotion() {
        ESP_LOGI(TAG, "CycleDisplayEmotion");
        // 开机后一段时间内禁止切换，避免上电抖动
        if (xTaskGetTickCount() < allow_switch_after_tick_) {
            return;
        }

        // 简单防抖：1200ms 内忽略重复触发
        static uint32_t last_switch_tick = 0;
        uint32_t now = xTaskGetTickCount();
        if (last_switch_tick != 0 && (now - last_switch_tick) < pdMS_TO_TICKS(1200)) {
            return;
        }

        last_switch_tick = now;

        if (display_ == nullptr) {
            ESP_LOGE(TAG, "CycleDisplayEmotion: display_ is nullptr");
            return;
        }

        // 定义所有可用的Display表情列表
        static const char* emotion_list[] = {
            "neutral",      // 中性
            "happy",        // 开心
            "laughing",     // 大笑
            "sad",          // 悲伤
            "angry",        // 愤怒
            "crying",       // 哭泣
            "loving",       // 喜爱
            "embarrassed",  // 尴尬
            "surprised",    // 惊讶
            "shocked",      // 震惊
            "thinking",     // 思考
            "winking",      // 眨眼
            "cool",         // 酷
            "relaxed",      // 放松
            "delicious",    // 美味
            "kissy",        // 亲吻
            "confident",    // 自信
            "sleepy",       // 困倦
            "silly",        // 傻笑
            "confused",     // 困惑
            "vertigo"       // 眩晕
        };
        const int emotion_list_size = sizeof(emotion_list) / sizeof(emotion_list[0]);
        
        // 使用静态变量记录当前表情索引
        static int current_emotion_index = 0;
        
        // 切换到下一个表情（循环）
        current_emotion_index = (current_emotion_index + 1) % emotion_list_size;
        const char* next_emotion = emotion_list[current_emotion_index];
        
        ESP_LOGI(TAG, "Switch display emotion: %d -> %d (%s)", 
                 (current_emotion_index - 1 + emotion_list_size) % emotion_list_size,
                 current_emotion_index, next_emotion);
        
        // 触发表情切换
        TriggerEmotion(next_emotion);
    }

    void CycleVideoGroup() {
        ESP_LOGI(TAG, "CycleVideoGroup");
        // 开机后一段时间内禁止切换，避免上电抖动
        if (xTaskGetTickCount() < allow_switch_after_tick_) {
            return;
        }

        // 简单防抖：1200ms 内忽略重复触发
        static uint32_t last_switch_tick = 0;
        uint32_t now = xTaskGetTickCount();
        if (last_switch_tick != 0 && (now - last_switch_tick) < pdMS_TO_TICKS(1200)) {
            return;
        }

        last_switch_tick = now;

        if (video_player_ == nullptr) {
            ESP_LOGE(TAG, "CycleVideoGroup: video_player_ is nullptr");
            return;
        }

        int cnt = video_player_->ReadVideoGroupCount();
        if (cnt <= 0) {
            ESP_LOGE(TAG, "CycleVideoGroup: No video groups available");
            return;
        }

        // 使用新的封装方式：基于 emotion 状态名称循环切换
        // 定义每个组对应的代表性 emotion（按照组索引顺序）
        static const char* emotion_list[] = {
            "happy",      // 组 0: 开心
            "neutral",    // 组 1: 中性
            "sad",       // 组 2: 悲伤
            "surprised", // 组 3: 惊讶
            "angry",     // 组 4: 愤怒
            "loving",    // 组 5: 爱心
            "thinking",  // 组 6: 思考
            "winking",   // 组 7: 眨眼
            "sleepy",    // 组 8: 睡眠
            "silly",     // 组 9: 傻笑
            "vertigo",   // 组 10: 眩晕
            "listen"     // 组 11: 聆听
        };
        const int emotion_list_size = sizeof(emotion_list) / sizeof(emotion_list[0]);
        
        // 获取当前播放的组索引
        int current_group = video_player_->GetCurrentGroupIndex();
        
        // 切换到下一个组（循环）
        int next_group = (current_group + 1) % cnt;
        
        // 确保 next_group 在 emotion_list 范围内
        if (next_group >= emotion_list_size) {
            next_group = 0;  // 超出范围则回到第一个
        }
        
        // 使用新的封装方式：通过 emotion 状态名称播放
        const char* next_emotion = emotion_list[next_group];
        video_player_->PlayVideoGroup(next_emotion);
        ESP_LOGI(TAG, "Switch video group: %d -> %d (%s)", current_group, next_group, next_emotion);
    }

    int MaxBacklightBrightness() {
        return 8;
    }

    // 显示电量圆环指示器（委托给EyeDisplay）
    void ShowBatteryIndicator() {
        if (display_ != nullptr) {
            // 如果在视频模式，停止视频播放并保存状态
            if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK && video_player_ != nullptr) {
                int current_group_index = video_player_->GetCurrentGroupIndex();
                ESP_LOGI(TAG, "ShowBatteryIndicator: 视频模式，停止播放，保存组索引: %d", current_group_index);
                // 在停止播放之前保存组索引
                saved_video_group_index_for_battery_ = current_group_index;
                video_player_->StopPlayback();
                // 延迟一下确保视频已停止
                vTaskDelay(pdMS_TO_TICKS(100));
                // 设置视频模式信息，以便5秒后恢复
                display_->SetVideoModeInfo(true, current_group_index);
            } else {
                // 非视频模式，清除视频模式信息
                saved_video_group_index_for_battery_ = -1;
                display_->SetVideoModeInfo(false, -1);
            }
            display_->ShowBatteryIndicator();
        }
    }
    
    // 隐藏电量圆环指示器（委托给EyeDisplay）
    void HideBatteryIndicator() {
        if (display_ != nullptr) {
            // 使用之前保存的视频组索引
            int saved_video_group_index = saved_video_group_index_for_battery_;
            ESP_LOGI(TAG, "HideBatteryIndicator: 开始，playback_mode_=%d, video_player_=%p, saved_video_group_index=%d", 
                     (int)playback_mode_, video_player_, saved_video_group_index);
            
            display_->HideBatteryIndicator();
            
            // 如果之前在视频模式，恢复视频播放
            if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK && video_player_ != nullptr && saved_video_group_index >= 0) {
                ESP_LOGI(TAG, "HideBatteryIndicator: 恢复视频播放，组索引: %d", saved_video_group_index);
                vTaskDelay(pdMS_TO_TICKS(100));  // 延迟一下确保UI已更新
                video_player_->PlayVideoGroupByIndex(saved_video_group_index);
                // 清除保存的索引
                saved_video_group_index_for_battery_ = -1;
            } else {
                ESP_LOGW(TAG, "HideBatteryIndicator: 不恢复视频播放 - playback_mode_=%d, video_player_=%p, saved_video_group_index=%d", 
                         (int)playback_mode_, video_player_, saved_video_group_index);
            }
        }
    }

    void InitializeChargingGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << STANDBY_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        gpio_config_t io_conf2 = {
            .pin_bit_mask = (1ULL << CHARGING_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf2));
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "first_level: %d", first_level);

        // 触摸按钮功能已屏蔽
        /*
        touch_button_.OnPressDown([this]() {
          
            ESP_LOGI(TAG, "touch_button_.OnPressDown");

            TickType_t current_time = xTaskGetTickCount();
            const TickType_t touch_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间
            
            // 检查是否已经过了冷却时间
            if (current_time - last_touch_time_ >= touch_cooldown) {
                last_touch_time_ = current_time; // 更新上次触发时间

                //切换表情
                if (CheckAndHandleEnterSleepMode()) {
                    // 交给休眠逻辑托管
                    ESP_LOGI(TAG, "触摸唤醒");
                    return;
                }
                // 根据当前模式触发表情
                if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                    // 视频模式：只切换视频组，不触发AI对话
                    this->CycleVideoGroup();
                } else {
                    // Display模式：显示表情并触发AI对话
                    TriggerEmotion("loving");

                    if (ChannelIsOpen()) {
                        #ifdef CONFIG_LANGUAGE_ZH_CN
                        Application::GetInstance().SendTextToAI("用户正在抚摸你");
                        #else
                        Application::GetInstance().SendTextToAI("User is touching you");
                        #endif
                    } else {
                        ESP_LOGI("touch", "Channel is not open");
                        Application::GetInstance().ToggleChatState();
                    }
                }
            } else {
                ESP_LOGI("touch", "Touch detected but in cooldown period");
            }
        });
        */

        // 创建双击检测定时器（使用esp_timer，避免栈溢出）
        if (!boot_button_timer_) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                    // 定时器超时，执行操作
                    if (board->boot_button_click_count_ == 1) {
                        // 单击：检查是否在显示二维码状态
                        if (board->qrcode_displaying_) {
                            // 如果正在显示二维码，根据当前模式决定操作
                            if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                                // 视频模式下，取消二维码并切换回Display模式
                                ESP_LOGI("MovecallMojiESP32S3", "单击检测 - 取消二维码，切换回Display模式");
                                board->ExitQrcodeAndEnterDisplayMode();
                            } else {
                                // Display模式下，取消二维码并切换到视频模式
                                ESP_LOGI("MovecallMojiESP32S3", "单击检测 - 取消二维码，进入视频模式");
                                board->ExitQrcodeAndEnterVideoMode();
                            }
                        } else if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                            // 视频模式下，单击切换视频组
                            ESP_LOGI("MovecallMojiESP32S3", "单击检测 - 视频模式下切换视频组");
                            board->CycleVideoGroup();
                        } else {
                            // Display模式下，单击：强制打断讲话，进入聆听模式（参考 gizwits-c2-6824-DRF-W300CA）
                            ESP_LOGI("MovecallMojiESP32S3", "单击检测 - 强制打断讲话，进入聆听模式");
                            auto& app = Application::GetInstance();
                            
                            // 强制打断：无论当前状态，都发送中止消息并重置解码器
                            app.AbortSpeaking(kAbortReasonNone);
                            app.ResetDecoder();
                            
                            // 调用 StartListening 进入聆听模式（它会处理所有状态）
                            app.StartListening();
                        }
                    } else if (board->boot_button_click_count_ == 2) {
                        // 双击：600ms内检测到第二次点击，且没有第三次点击（定时器超时）
                        // 显示电量圆环
                        ESP_LOGI("MovecallMojiESP32S3", "双击检测 - 显示电量");
                        board->ShowBatteryIndicator();
                    } else if (board->boot_button_click_count_ == 3) {
                        // 三击：600ms内检测到第三次点击，且没有第四次点击（定时器超时）
                        // 直接进入配网模式（重启），不显示二维码
                        ESP_LOGI("MovecallMojiESP32S3", "三击检测 - 直接进入配网模式");
                        board->InnerResetWifiConfiguration();
                    }
                    // 注意：四击在 OnClick 回调中立即处理，不会到达这里
                    board->boot_button_click_count_ = 0;
                },
                .arg = this,
                .name = "boot_btn_timer"
            };
            esp_timer_create(&timer_args, &boot_button_timer_);
        }
        
        // 创建二维码定时器（用于自动进入配网模式）
        if (!qrcode_timer_) {
            esp_timer_create_args_t qrcode_timer_args = {
                .callback = [](void* arg) {
                    MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                    ESP_LOGI("MovecallMojiESP32S3", "二维码显示超时，进入配网模式");
                    board->qrcode_displaying_ = false;
                    board->InnerResetWifiConfiguration();
                },
                .arg = this,
                .name = "qrcode_timer"
            };
            esp_timer_create(&qrcode_timer_args, &qrcode_timer_);
        }
        
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
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
                // vTaskDelay(pdMS_TO_TICKS(200));
                // auto codec = GetAudioCodec();
                // codec->EnableOutput(true);
                // Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                this->GetBacklight()->SetBrightness(0, false);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            // InnerResetWifiConfiguration();

            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            
            // 双击检测：使用 OnClick 来检测（参考 gizwits-c2-6824-DRF-W300CA 的实现）
            // 注意：三击配网由按钮库的 OnMultipleClick 处理，这里只处理单击和双击
            
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    // 注释掉旧的表情动画，改用视频播放，节省内存
                    // board->display_->SetEmotion("neutral");

                    if (board->IsCharging()) {
                        // 充电中，只关闭背光
                        board->GetBacklight()->SetBrightness(0, false);
                        board->is_charging_sleep_ = true;
                        Application::GetInstance().QuitTalking();
                    } else {
                        // 没有充电，关机
                        board->PowerOff();
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        // 单击、双击和三击检测
        boot_button_.OnClick([this]() {
            int64_t now_ms = esp_timer_get_time() / 1000;
            const int64_t TRIPLE_CLICK_WINDOW_MS = 1000;  // 三击时间窗口1000ms（增加时间窗口）
            const int64_t DOUBLE_CLICK_WINDOW_MS = kDoubleClickWindowMs;  // 使用成员变量的双击窗口时间（800ms）
            
            // 如果距离上次点击超过三击窗口，重置计数器
            if (now_ms - boot_button_last_click_ms_ > TRIPLE_CLICK_WINDOW_MS) {
                boot_button_click_count_ = 0;
                ESP_LOGI(TAG, "boot_button_.OnClick - 重置计数器（超过时间窗口）");
            }
            boot_button_last_click_ms_ = now_ms;
            
            int64_t time_since_last_click = (boot_button_click_count_ > 0) ? (now_ms - boot_button_last_click_ms_) : 0;
            boot_button_click_count_++;
            ESP_LOGI(TAG, "boot_button_.OnClick - 点击次数: %d, 距离上次点击: %lld ms", 
                     boot_button_click_count_, time_since_last_click);
            
            // 停止之前的定时器（如果有）
            if (boot_button_timer_ != nullptr) {
                esp_timer_stop(boot_button_timer_);
            }
            
            if (boot_button_click_count_ == 1) {
                // 第一次点击，启动定时器（800ms后执行单击操作）
                ESP_LOGI(TAG, "第一次点击，启动定时器 %d ms", DOUBLE_CLICK_WINDOW_MS);
                esp_timer_start_once(boot_button_timer_, DOUBLE_CLICK_WINDOW_MS * 1000);
            } else if (boot_button_click_count_ == 2) {
                // 第二次点击，停止定时器，启动更短的定时器（1000ms后执行双击操作 - 显示电量，给三击留时间）
                ESP_LOGI(TAG, "第二次点击，停止定时器，启动三击窗口定时器 %d ms", TRIPLE_CLICK_WINDOW_MS);
                esp_timer_stop(boot_button_timer_);
                esp_timer_start_once(boot_button_timer_, TRIPLE_CLICK_WINDOW_MS * 1000);  // 使用三击窗口时间
            } else if (boot_button_click_count_ == 3) {
                // 第三次点击，停止定时器，启动更短的定时器（1000ms后执行三击操作，给四击留时间）
                ESP_LOGI(TAG, "第三次点击，停止定时器，启动三击窗口定时器 %d ms", TRIPLE_CLICK_WINDOW_MS);
                esp_timer_stop(boot_button_timer_);
                esp_timer_start_once(boot_button_timer_, TRIPLE_CLICK_WINDOW_MS * 1000);  // 使用三击窗口时间
            } else if (boot_button_click_count_ >= 4) {
                // 第四次或更多次点击，立即停止定时器并执行四击操作（直接切换模式，不显示二维码）
                ESP_LOGI(TAG, "四击检测 - 直接切换模式");
                esp_timer_stop(boot_button_timer_);
                boot_button_click_count_ = 0;
                SwitchPlaybackMode();
            }
        });

        // 注意：OnClick 已经在上面注册过了，这里不需要重复注册
        // 如果重复注册会覆盖之前的处理
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker")); 
        thing_manager.AddThing(iot::CreateThing("Screen"));   
    }
    void InitializeGpio(gpio_num_t gpio_num_, bool output = false, bool open_drain = false) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = open_drain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 开漏输出通常需要上拉
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
    
    // 显示二维码（不重启，直接显示）
    void ShowQrcode() {
        ESP_LOGI(TAG, "ShowQrcode: 显示二维码");
        
        // 先停止视频播放（如果正在播放），避免访问已删除的对象
        if (video_player_ != nullptr && playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
            video_player_->StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(200));  // 等待视频任务完全退出
        }
        
        if (display_ != nullptr) {
            display_->EnterWifiConfig();
            qrcode_displaying_ = true;
        }
    }
    
    // 取消二维码并切换回Display模式
    void ExitQrcodeAndEnterDisplayMode() {
        ESP_LOGI(TAG, "ExitQrcodeAndEnterDisplayMode: 取消二维码，切换回Display模式");
        qrcode_displaying_ = false;
        // 取消二维码定时器
        if (qrcode_timer_ != nullptr) {
            esp_timer_stop(qrcode_timer_);
        }
        
        // 先停止视频播放（如果正在播放）
        if (video_player_ != nullptr) {
            video_player_->StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(200));  // 等待视频任务完全退出
        }
        
        // 清空屏幕，恢复黑色背景
        if (display_ != nullptr) {
            if (display_->Lock(1000)) {
                lv_obj_t* screen = lv_screen_active();
                if (screen != nullptr) {
                    // 先删除所有子对象，但保留屏幕对象本身
                    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                    for (int32_t i = child_cnt - 1; i >= 0; i--) {
                        lv_obj_t* child = lv_obj_get_child(screen, i);
                        if (child != nullptr) {
                            lv_obj_del(child);
                        }
                    }
                    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
                }
                display_->Unlock();
            }
        }
        
        // 等待一下确保屏幕清理完成和LVGL稳定
        vTaskDelay(pdMS_TO_TICKS(300));
        
        // 切换到Display模式
        if (playback_mode_ != PlaybackMode::DISPLAY_ANIMATION) {
            playback_mode_ = PlaybackMode::DISPLAY_ANIMATION;
            ESP_LOGI(TAG, "切换到Display动画模式");
            
            // Display模式下恢复唤醒词检测（根据设备状态）
            auto& app = Application::GetInstance();
            auto device_state = app.GetDeviceState();
            ESP_LOGI(TAG, "切换回Display模式，当前设备状态: %d", device_state);
            
            // 延迟一下，确保所有状态更新完成后再恢复唤醒词检测
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // 重新获取设备状态（可能已经改变）
            device_state = app.GetDeviceState();
            ESP_LOGI(TAG, "切换回Display模式，延迟后设备状态: %d", device_state);
            
            // 如果设备状态是idle或sleeping，恢复唤醒词检测
            if (device_state == kDeviceStateIdle || device_state == kDeviceStateSleeping) {
                app.GetAudioService().EnableWakeWordDetection(true);
                app.GetAudioService().EnableVoiceProcessing(false);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（idle/sleeping状态）");
            } else if (device_state == kDeviceStateListening || device_state == kDeviceStateSpeaking) {
                // 在listening或speaking状态下，也需要启用唤醒词检测（可以在speaking时打断）
                #if CONFIG_USE_AFE_WAKE_WORD
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（listening/speaking状态，AFE唤醒词）");
                #else
                // 非AFE唤醒词，在speaking状态下不启用唤醒词检测
                if (device_state == kDeviceStateListening) {
                    app.GetAudioService().EnableWakeWordDetection(true);
                    ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（listening状态）");
                } else {
                    ESP_LOGW(TAG, "Display模式：speaking状态下非AFE唤醒词不启用唤醒词检测");
                }
                #endif
            } else {
                // 其他状态，也尝试启用唤醒词检测（如果设备允许）
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（其他状态）");
            }
            
            // 再次等待，确保所有操作完成和屏幕稳定
            vTaskDelay(pdMS_TO_TICKS(200));
            
            // 重新初始化显示动画（屏幕已经清空）
            if (display_ != nullptr) {
                display_->SetEmotion("neutral");
            }
        }
    }
    
    // 取消二维码并进入视频模式
    void ExitQrcodeAndEnterVideoMode() {
        ESP_LOGI(TAG, "ExitQrcodeAndEnterVideoMode: 取消二维码，进入视频模式");
        qrcode_displaying_ = false;
        // 取消二维码定时器
        if (qrcode_timer_ != nullptr) {
            esp_timer_stop(qrcode_timer_);
        }
        
        // 先停止AI对话和音乐播放
        auto& app = Application::GetInstance();
        app.QuitTalking();  // 停止AI对话，关闭音频通道
        app.CancelPlayMusic();  // 停止音乐播放
        ESP_LOGI(TAG, "已停止AI对话和音乐播放");
        
        // 等待一下确保音频操作完全停止
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 先停止任何正在进行的视频播放
        if (video_player_ != nullptr) {
            video_player_->StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(200));  // 等待视频任务完全退出
        }
        
        // 清空屏幕，恢复黑色背景
        if (display_ != nullptr) {
            if (display_->Lock(1000)) {
                lv_obj_t* screen = lv_screen_active();
                if (screen != nullptr) {
                    // 先删除所有子对象，但保留屏幕对象本身
                    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                    for (int32_t i = child_cnt - 1; i >= 0; i--) {
                        lv_obj_t* child = lv_obj_get_child(screen, i);
                        if (child != nullptr) {
                            lv_obj_del(child);
                        }
                    }
                    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
                }
                display_->Unlock();
            }
        }
        
        // 等待一下确保屏幕清理完成和LVGL稳定
        vTaskDelay(pdMS_TO_TICKS(300));
        
        // 切换到视频模式（不调用SwitchPlaybackMode，直接设置状态和启动视频）
        if (playback_mode_ != PlaybackMode::VIDEO_PLAYBACK) {
            playback_mode_ = PlaybackMode::VIDEO_PLAYBACK;
            ESP_LOGI(TAG, "切换到视频播放模式");
            
            // 视频模式下禁用唤醒词检测和语音处理（离线模式）
            auto& app = Application::GetInstance();
            app.GetAudioService().EnableWakeWordDetection(false);
            app.GetAudioService().EnableVoiceProcessing(false);
            ESP_LOGI(TAG, "视频模式：已禁用唤醒词检测和语音处理");
            
            // 检查充电状态，如果正在充电，显示电量圆环（因为状态改变回调可能不会触发）
            if (IsCharging()) {
                ESP_LOGI(TAG, "ExitQrcodeAndEnterVideoMode: 检测到正在充电，显示电量圆环");
                auto device_state = app.GetDeviceState();
                if (device_state != kDeviceStateWifiConfiguring && display_ != nullptr) {
                    int battery_level = 0;
                    bool charging = false;
                    bool discharging = false;
                    if (GetBatteryLevel(battery_level, charging, discharging)) {
                        display_->ShowBatteryIndicatorForCharging(battery_level);
                    } else {
                        display_->ShowBatteryIndicatorForCharging();
                    }
                }
            }
            
            // 再次等待，确保所有操作完成和屏幕稳定
            vTaskDelay(pdMS_TO_TICKS(200));
            
            // 启动视频播放（屏幕已经清空，不需要隐藏对象）
            if (video_player_ != nullptr) {
                video_player_->PlayVideoGroup("neutral");
            }
        }
    }

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
    }
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
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &lis2hh12_i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create LIS2HH12 I2C bus: %s", esp_err_to_name(ret));
            return;
        }
        
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = LIS2HH12_I2C_ADDR,
            .scl_speed_hz = 50000,  // 降低到50kHz，提高稳定性
        };
        ret = i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &lis2hh12_dev_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add LIS2HH12 device: %s", esp_err_to_name(ret));
            return;
        }
        
        ESP_LOGI(TAG, "LIS2HH12 I2C initialized successfully");
    }

    void InitializeLis2hh12() {
        // 首先检测设备是否存在
        uint8_t who_am_i = this->lis2hh12_read_reg(0x0F); // WHO_AM_I寄存器
        ESP_LOGI(TAG, "LIS2HH12 WHO_AM_I: 0x%02X", who_am_i);
        
        if (who_am_i != 0x41) { // LIS2HH12的WHO_AM_I值应该是0x41
            ESP_LOGE(TAG, "LIS2HH12 not found! Expected 0x41, got 0x%02X", who_am_i);
            return;
        }
        
        ESP_LOGI(TAG, "LIS2HH12 detected successfully");
        
        // 0x20: CTRL1, 0x57 = 100Hz, all axes enable, normal mode
        this->lis2hh12_write_reg(0x20, 0x57);
        // 0x23: CTRL4, 0x00 = continuous update, LSB at lower address
        this->lis2hh12_write_reg(0x23, 0x00);
        
        ESP_LOGI(TAG, "LIS2HH12 initialized successfully");
    }

    // LIS2HH12 I2C读写成员函数
    uint8_t lis2hh12_read_reg(uint8_t reg) {
        uint8_t data = 0;
        esp_err_t ret = i2c_master_transmit_receive(lis2hh12_dev_, &reg, 1, &data, 1, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            // ESP_LOGE(TAG, "LIS2HH12 read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
            return 0;
        }
        return data;
    }
    
    void lis2hh12_write_reg(uint8_t reg, uint8_t value) {
        uint8_t buf[2] = {reg, value};
        esp_err_t ret = i2c_master_transmit(lis2hh12_dev_, buf, 2, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LIS2HH12 write reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
        }
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
            // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(GetDisplay());
            if (is_charging) {
                // 充电开始时的处理逻辑
                ESP_LOGI(TAG, "检测到开始充电");
                // 降低发热                
                GetBacklight()->SetBrightness(5, false);
                
                // 检查设备状态，除了配网模式，其他状态都显示电量圆环
                auto& app = Application::GetInstance();
                auto device_state = app.GetDeviceState();
                ESP_LOGI(TAG, "充电时设备状态: %d", device_state);
                
                // 只排除配网模式，其他所有状态都显示电量圆环
                if (device_state != kDeviceStateWifiConfiguring) {
                    ESP_LOGI(TAG, "显示充电电量圆环");
                    if (display_ != nullptr) {
                        // 在调用 ShowBatteryIndicatorForCharging() 之前，先获取电量
                        // 这样 ShowBatteryIndicatorForCharging() 就可以使用传入的电量值，避免在定时器回调中调用 Board::GetInstance()
                        int battery_level = 0;
                        bool charging = false;
                        bool discharging = false;
                        if (GetBatteryLevel(battery_level, charging, discharging)) {
                            display_->ShowBatteryIndicatorForCharging(battery_level);
                        } else {
                            display_->ShowBatteryIndicatorForCharging();
                        }
                        ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging调用完成");
                    }
                }
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");
                
                // 隐藏充电时的电量圆环
                if (display_ != nullptr) {
                    display_->HideBatteryIndicatorForCharging();
                }

                if (this->is_charging_sleep_) {
                    ESP_LOGI(TAG, "充电停止，关机");
                    PowerOff();
                }
            }

            // 通知 mqtt 
            auto& mqtt_client = MqttClient::getInstance();
            mqtt_client.ReportTimer();
        });
        
        // 注册设备状态改变回调，用于在充电时根据状态显示/隐藏电量圆环
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState prev, DeviceState curr) {
                ESP_LOGI(TAG, "DeviceStateEventManager回调: prev=%d, curr=%d", prev, curr);
                
                // 强制刷新一次充电状态检测，确保状态是最新的
                if (power_manager_ != nullptr) {
                    power_manager_->CheckBatteryStatusImmediately();
                }
                
                // 如果正在充电，根据新状态决定是否显示电量圆环
                bool is_charging = IsCharging();
                ESP_LOGI(TAG, "DeviceStateEventManager回调: is_charging=%d (强制刷新后)", is_charging);
                if (is_charging) {
                    // 只排除配网模式，其他所有状态都显示电量圆环
                    if (curr != kDeviceStateWifiConfiguring) {
                        // 新状态允许显示电量圆环
                        ESP_LOGI(TAG, "DeviceStateEventManager回调: 显示充电电量圆环 (device_state=%d, display_=%p)", 
                                 curr, display_);
                        if (display_ != nullptr) {
                            int battery_level = 0;
                            bool charging = false;
                            bool discharging = false;
                            if (GetBatteryLevel(battery_level, charging, discharging)) {
                                display_->ShowBatteryIndicatorForCharging(battery_level);
                            } else {
                                display_->ShowBatteryIndicatorForCharging();
                            }
                            ESP_LOGI(TAG, "DeviceStateEventManager回调: ShowBatteryIndicatorForCharging调用完成");
                        } else {
                            ESP_LOGE(TAG, "DeviceStateEventManager回调: display_为nullptr，无法显示圆环");
                        }
                    } else {
                        // 配网模式，隐藏充电时的电量圆环
                        ESP_LOGI(TAG, "DeviceStateEventManager回调: 隐藏充电电量圆环（配网模式）");
                        if (display_ != nullptr) {
                            display_->HideBatteryIndicatorForCharging();
                        }
                    }
                } else {
                    ESP_LOGI(TAG, "DeviceStateEventManager回调: 未充电，不显示电量圆环");
                }
            }
        );
    }

    void InitializeFlash() {
        auto& flash = W25Q64Flash::GetInstance();
         // 初始化 Flash
        esp_err_t flash_ret = flash.Initialize(FLASH_PIN_MOSI, FLASH_PIN_MISO, 
            FLASH_PIN_CLK, FLASH_PIN_CS, 10000);
        if (flash_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Flash: %s", esp_err_to_name(flash_ret));
        // return;
        } else {
        ESP_LOGI(TAG, "Flash initialized successfully!");
        }
    }

public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO) {  // 触摸按钮已屏蔽（保留初始化，但回调已注释） 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);

        // 设置I2C master日志级别为ERROR，忽略I2C事务失败的日志
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        
        InitializeChargingGpio();

        InitializeGpio(POWER_GPIO, true);
        InitializeGpio(GPIO_NUM_1, true, true);  // 开漏输出

        InitializeI2c();
        InitializeGpio(AUDIO_CODEC_PA_PIN, true);
        // InitializeGpio(DISPLAY_BACKLIGHT_PIN, false);
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeLis2hh12I2c(); // 初始化LIS2HH12专用I2C
        InitializeLis2hh12();    // 初始化LIS2HH12陀螺仪
        
        // 检查I2C设备是否正常
        // if (lis2hh12_dev_ == nullptr) {
        //     ESP_LOGE(TAG, "LIS2HH12 device not initialized, skipping sensor task");
        // } else {
        //     ESP_LOGI(TAG, "LIS2HH12 device initialized successfully");
        // }
        InitializeButtons();
        InitializeIot();
        // xTaskCreatePinnedToCore(MovecallMojiESP32S3::lis2hh12_task, "lis2hh12_task", 1024 * 3, this, 1, NULL, 0); // 启动检测任务 - 已注释，不启动陀螺仪任务
        InitializePowerManager();
        InitializePowerSaveTimer();
        // ESP_LOGI(TAG, "ReadADC2_CH1_Oneshot");
        // ReadADC2_CH1_Oneshot();
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
            
            // 延迟一下，等待ADC稳定，然后检查充电状态
            // 如果启动时就在充电，状态改变回调不会触发，需要主动检查
            vTaskDelay(pdMS_TO_TICKS(500));  // 延迟500ms等待ADC稳定
            power_manager_->CheckBatteryStatusImmediately();  // 再次检测，确保状态更新
            
            // 检查充电状态，如果正在充电，显示电量圆环
            if (IsCharging()) {
                ESP_LOGI(TAG, "启动时检测到正在充电，显示电量圆环");
                auto& app = Application::GetInstance();
                auto device_state = app.GetDeviceState();
                if (device_state != kDeviceStateWifiConfiguring && display_ != nullptr) {
                    int battery_level = 0;
                    bool charging = false;
                    bool discharging = false;
                    if (GetBatteryLevel(battery_level, charging, discharging)) {
                        display_->ShowBatteryIndicatorForCharging(battery_level);
                    } else {
                        display_->ShowBatteryIndicatorForCharging();
                    }
                    ESP_LOGI(TAG, "启动时显示充电电量圆环完成");
                } else {
                    ESP_LOGI(TAG, "启动时不显示充电电量圆环 (device_state=%d, display_=%p)", 
                             device_state, display_);
                }
            } else {
                ESP_LOGI(TAG, "启动时未检测到充电状态");
            }
        }

        InitializeFlash();

        xTaskCreate(
            RestoreBacklightTask,      // 任务函数
            "restore_backlight",       // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );

        // 开机：根据当前模式决定是否启动视频播放
        // 默认是 DISPLAY_ANIMATION 模式，不启动视频播放
        allow_switch_after_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        
        // 如果默认模式是视频模式，才启动视频播放（但默认是 DISPLAY_ANIMATION，所以不会启动）
        // 这里保持为空，让用户通过短按两下切换到视频模式

        if (Application::GetInstance().IsTmpFactoryTestMode()) {
            display_->EnterTestMode();
            display_->SetTestItems(test_items);
            // 开始产测模式

            Application::GetInstance().Schedule([this]() {
                display_->StartRGBTest();
                vTaskDelay(pdMS_TO_TICKS(9000));
                display_->StopRGBTest();
            }, "factory_test_mode");

            Application::GetInstance().Schedule([this]() {
                // 尝试连接产测路由wifi
                auto& wifi_station = WifiStation::GetInstance();
                wifi_station.Start();

                ESP_LOGI(TAG, "产测模式临时连接产测路由器");
                auto& wifi_manager = WifiConnectionManager::GetInstance();
                esp_err_t ret = wifi_manager.Connect(CONFIG_PRODUCT_TEST_WIFI, CONFIG_PRODUCT_TEST_WIFI_PASSWORD);
                ESP_LOGI(TAG, "产测模式临时连接产测路由器 ret: %d", ret);
                if (ret == ESP_OK) {
                    display_->UpdateTestItemStatus("wifi", 1);
                } else {
                    // 设置失败
                    display_->UpdateTestItemStatus("wifi", 2);
                }
            }, "factory_test_mode");

            Application::GetInstance().Schedule([this]() {
                // ADC 电池检测
                vTaskDelay(pdMS_TO_TICKS(1000));
                int level = 0;
                bool charging = false;
                bool discharging = false;
                GetBatteryLevel(level, charging, discharging);
                // 合理范围：1..100 认为有效（0 可能意味着未接电池/异常）
                if (level >= 1 && level <= 100) {
                    display_->UpdateTestItemStatus("battery", 1);
                } else {
                    display_->UpdateTestItemStatus("battery", 2);
                }
            }, "adc_test");
        }
    }

    virtual void PowerOff() override {
        gpio_set_level(POWER_GPIO, 0);
    }

    virtual void WakeWordDetected() override {
        // 视频模式下禁用唤醒词检测和AI对话
        if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
            ESP_LOGI(TAG, "WakeWordDetected: 视频模式下忽略唤醒词");
            return;
        }
        
        ESP_LOGI(TAG, "WakeWordDetected");
        display_->UpdateTestItemStatus("mic", 1);

        GetAudioCodec()->EnableOutput(true);
        Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
    }

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
        int level;
        bool charging, discharging;
        self->GetBatteryLevel(level, charging, discharging);
        // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(self->GetDisplay());
        self->GetBacklight()->RestoreBrightness();

        // xunguan_display->StartAutoTest(1000);

        // if (charging) {
        //     // 降低发热            
        //     // xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::POWER_SAVE);
        //     xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);
        // } else {
        //     xunguan_display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);
        // }
        vTaskDelete(NULL); // 任务结束时删除自己
    }

    virtual Display* GetDisplay() override {
        // 返回包装器，以便拦截SetEmotion调用
        if (display_wrapper_ == nullptr && display_ != nullptr) {
            display_wrapper_ = new DisplayWrapper(display_, this);
        }
        return display_wrapper_ != nullptr ? static_cast<Display*>(display_wrapper_) : display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool IsCharging() override {
        ESP_LOGI(TAG, "IsCharging: 开始, power_manager_=%p", power_manager_);
        // 使用PowerManager检测充电状态（通过ADC2_CH2检测VDD电压）
        if (power_manager_ != nullptr) {
            bool result = power_manager_->IsChargingByVdd();
            ESP_LOGI(TAG, "IsCharging: 返回 %d", result);
            return result;
        }
        ESP_LOGW(TAG, "IsCharging: power_manager_为nullptr，返回false");
        return false;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        ESP_LOGI(TAG, "GetBatteryLevel: 开始");
        ESP_LOGI(TAG, "GetBatteryLevel: 调用IsCharging前");
        charging = IsCharging();
        ESP_LOGI(TAG, "GetBatteryLevel: 调用IsCharging后, charging=%d", charging);
        discharging = !charging;
        // 使用PowerManager获取真实电量（基于ADC2_CH1的数据）
        if (power_manager_ != nullptr) {
            ESP_LOGI(TAG, "GetBatteryLevel: 调用power_manager_->GetBatteryLevel前");
            level = power_manager_->GetBatteryLevel();
            ESP_LOGI(TAG, "GetBatteryLevel: 调用power_manager_->GetBatteryLevel后, level=%d", level);
        } else {
            ESP_LOGW(TAG, "GetBatteryLevel: power_manager_为nullptr，使用默认值100");
            level = 100;  // 如果PowerManager未初始化，返回默认值
        }
        ESP_LOGI(TAG, "GetBatteryLevel: 返回, level: %d, charging: %d, discharging: %d", level, charging, discharging);
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

// DisplayWrapper::SetEmotion 的实现（需要在 MovecallMojiESP32S3 类定义之后）
void DisplayWrapper::SetEmotion(const char* emotion) {
    if (board_ != nullptr) {
        board_->TriggerEmotion(emotion);
    } else if (wrapped_display_ != nullptr) {
        wrapped_display_->SetEmotion(emotion);
    }
}

DECLARE_BOARD(MovecallMojiESP32S3);