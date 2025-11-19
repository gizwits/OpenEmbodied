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

// 休眠时间配置（单位：秒）
// 20分钟 = 60 * 20 = 1200秒
#define SLEEP_TIME_SEC (30 * 1)
// 关机时间配置（单位：秒）
// 30分钟 = 60 * 30 = 1800秒
#define SHUTDOWN_TIME_SEC (60 * 30)

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

#define LIS2HH12_I2C_ADDR 0x1E  // SDO接GND为0x1D，接VDD为0x1E（实际硬件是0x1E）
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
    
    // 视频模式轮播相关
    esp_timer_handle_t video_cycle_timer_ = nullptr;  // 视频轮播定时器
    bool video_cycling_ = false;  // 是否正在轮播
    esp_timer_handle_t gyro_emotion_timer_ = nullptr;  // 陀螺仪触发表情的恢复定时器（5秒后恢复轮播）
    bool gyro_emotion_active_ = false;  // 是否正在显示陀螺仪触发的表情
    const char* last_gyro_emotion_ = nullptr;  // 上次陀螺仪触发的表情，用于避免重复触发
    
    // 双击显示电量时保存的状态
    bool was_video_cycling_before_battery_ = false;  // 显示电量前是否在轮播

    std::vector<TestItem> test_items = {
        {"lcd", "LCD测试", 1},
        {"key", "按键测试", 0},
        {"wifi", "WiFi连接测试", 0},
        {"sensor", "陀螺仪测试", 0},
        {"battery", "电池检测", 0},
        {"mic", "麦克风检测", 0},
    };


    void InitializePowerSaveTimer() {
        // 使用宏定义配置休眠和关机时间
        // SLEEP_TIME_SEC: 进入睡眠的时间（默认20分钟）
        // SHUTDOWN_TIME_SEC: 关机时间（默认30分钟）
        power_save_timer_ = new PowerSaveTimer(-1, SLEEP_TIME_SEC, SHUTDOWN_TIME_SEC);
        // power_save_timer_ = new PowerSaveTimer(-1, 20 * 1, 60 * 2);  // 测试用：20秒休眠，2分钟关机
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGE(TAG, "Enabling sleep mode");
            
            // 先停止视频播放和轮播，避免视频任务阻塞休眠
            if (video_player_ != nullptr) {
                ESP_LOGI(TAG, "进入休眠模式前，停止视频播放");
                // 如果正在轮播，保存状态以便唤醒后恢复
                bool was_cycling = video_cycling_;
                if (was_cycling) {
                    ESP_LOGI(TAG, "进入休眠模式前，停止视频轮播（唤醒后恢复）");
                    StopVideoCycling();
                }
                video_player_->StopPlayback();
                // 等待视频任务完全退出
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            
            if(IsCharging()) {
                // 充电中
                is_charging_sleep_ = true;
                Application::GetInstance().Schedule([this]() {
                    Application::GetInstance().QuitTalking();
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);

                    // 在视频模式下显示睡觉表情（组8对应sleepy）
                    if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK && video_player_ != nullptr) {
                        ESP_LOGI(TAG, "进入休眠模式：显示睡觉表情（组8）");
                        // 停止轮播，只播放睡觉表情，循环播放
                        video_player_->SetLoopGroup(true);
                        video_player_->PlayVideoGroupByIndex(8);  // 组8对应sleepy
                    } else if (display_ != nullptr) {
                        // Display模式下使用传统方式
                        display_->SetEmotion("sleepy");
                    }
                }, "EnterSleepMode_QuitTalking");

            } else {
                // 关闭 wifi，进入待机模式
                Application::GetInstance().EnterSleepMode();
            }
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGE(TAG, "退出休眠模式");
            // 如果之前在视频模式，恢复视频播放
            if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK && video_player_ != nullptr) {
                ESP_LOGI(TAG, "退出休眠模式后，恢复视频播放");
                // 先停止当前的睡觉表情（如果正在播放）
                video_player_->StopPlayback();
                vTaskDelay(pdMS_TO_TICKS(200));
                // 重新启动轮播（从组0开始）
                StartVideoCycling();
            }
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
        float last_total_accel = 0.0f;
        const float threshold = 0.45f; // g-force
        int shake_count = 0;
        const int shake_count_threshold = 3; // 连续3次检测到变化才触发
        const int shake_count_decay = 1;     // 每次没检测到就-1
        TickType_t last_shake_time = 0;      // 上次摇晃触发时间
        const TickType_t shake_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间（增加冷却时间，降低灵敏度）
        int debug_counter = 0;  // 调试计数器，每10次打印一次数据（用于校准轴方向）
        
        // 轴校准参数（根据实际测试数据设置）
        // 静止状态基准值（用于判断方向）
        const int16_t x_rest = 4000;      // X轴静止时约3000-5000
        const int16_t y_rest = 500;       // Y轴静止时约300-800
        const int16_t z_rest = -15500;   // Z轴静止时约-15000（重力方向）
        
        // 方向检测阈值（raw值）
        const int16_t x_turn_threshold = 8000;   // X轴变化超过此值判断为转向（左转X<0，右转X>0，但根据数据左转X变负）
        const int16_t y_forward_threshold = 1000;  // Y轴正数超过此值判断为前进
        const int16_t y_backward_threshold = -3000; // Y轴负数超过此值判断为后退/右转倾角
        
        // 方向检测状态
        int16_t last_x_raw = x_rest;
        int16_t last_y_raw = y_rest;
        TickType_t last_direction_time = 0;
        const TickType_t direction_cooldown = pdMS_TO_TICKS(2000); // 方向检测冷却时间2秒
        
        // ESP_LOGI("LIS2HH12", "陀螺仪检测任务启动");
        // ESP_LOGI("LIS2HH12", "=== 轴校准模式：每10次采样打印一次数据 ===");
        // ESP_LOGI("LIS2HH12", "格式: [X轴] [Y轴] [Z轴] | 原始值(raw) | g值 | 总加速度 | 变化量");
        // ESP_LOGI("LIS2HH12", "方向检测阈值: X轴转向=%d, Y轴前进=%d, Y轴后退=%d", 
        //          x_turn_threshold, y_forward_threshold, y_backward_threshold);
        
        while (1) {
            // 读取X/Y/Z加速度数据（LIS2HH12使用±2g量程，灵敏度为0.061 mg/LSB）
            int16_t x_raw = (int16_t)((board->lis2hh12_read_reg_pub(0x29) << 8) | board->lis2hh12_read_reg_pub(0x28));
            int16_t y_raw = (int16_t)((board->lis2hh12_read_reg_pub(0x2B) << 8) | board->lis2hh12_read_reg_pub(0x2A));
            int16_t z_raw = (int16_t)((board->lis2hh12_read_reg_pub(0x2D) << 8) | board->lis2hh12_read_reg_pub(0x2C));
            
            // 转换为g值：±2g量程，16位数据，灵敏度0.061 mg/LSB = 0.000061 g/LSB
            // 所以转换公式：g = raw * 0.000061 * 2 / 32768 = raw * 0.061 / 1000
            float ax = x_raw * 0.061f / 1000.0f;
            float ay = y_raw * 0.061f / 1000.0f;
            float az = z_raw * 0.061f / 1000.0f;
            
            // 计算总加速度（向量长度）：sqrt(ax^2 + ay^2 + az^2)
            float total_accel = sqrtf(ax * ax + ay * ay + az * az);
            
            // 计算总加速度的变化量（更准确反映摇晃）
            float delta_total = fabs(total_accel - last_total_accel);
            
            // 调试计数器：每10次打印一次，用于校准轴方向
            // debug_counter++;
            // if (debug_counter >= 10) {
            //     debug_counter = 0;
            //     ESP_LOGI("LIS2HH12", "=== 轴数据 ===");
            //     ESP_LOGI("LIS2HH12", "X轴: raw=%6d, g=%7.3f | Y轴: raw=%6d, g=%7.3f | Z轴: raw=%6d, g=%7.3f", 
            //              x_raw, ax, y_raw, ay, z_raw, az);
            //     ESP_LOGI("LIS2HH12", "总加速度=%.3f g, 变化量=%.3f g, 摇晃计数=%d", 
            //              total_accel, delta_total, shake_count);
            //     ESP_LOGI("LIS2HH12", "提示: 静止时总加速度应接近1.0g（重力），移动时观察哪个轴变化最大");
            // }
            
            // 方向检测：根据轴的变化判断方向（优先于摇晃检测）
            // 暂时注释掉，先调通整体逻辑
            /*
            TickType_t current_time = xTaskGetTickCount();
            if (current_time - last_direction_time >= direction_cooldown) {
                const char* detected_emotion = nullptr;
                
                // 检测左转：X轴从正数变为负数（或负数绝对值很大）
                if (x_raw < -x_turn_threshold || (x_raw < 0 && last_x_raw > 0 && abs(x_raw - last_x_raw) > x_turn_threshold)) {
                    ESP_LOGI("LIS2HH12", "=== 方向检测：左转 ===");
                    ESP_LOGI("LIS2HH12", "检测到左转: X轴=%d (变化=%d), 触发表情: Turn_left -> 组12", x_raw, x_raw - last_x_raw);
                    last_direction_time = current_time;
                    detected_emotion = "Turn_left";
                }
                // 检测右转倾角：Y轴变为较大的负值
                else if (y_raw < y_backward_threshold) {
                    ESP_LOGI("LIS2HH12", "=== 方向检测：右转倾角 ===");
                    ESP_LOGI("LIS2HH12", "检测到右转倾角: Y轴=%d (变化=%d), 触发表情: Turn_right -> 组13", y_raw, y_raw - last_y_raw);
                    last_direction_time = current_time;
                    detected_emotion = "Turn_right";
                }
                // 检测前进倾角：Y轴变为较大的正值 -> 触发加速表情
                else if (y_raw > y_forward_threshold) {
                    ESP_LOGI("LIS2HH12", "=== 方向检测：前进倾角 ===");
                    ESP_LOGI("LIS2HH12", "检测到前进倾角: Y轴=%d (变化=%d), 触发表情: Accelerate -> 组14 (加速表情)", 
                             y_raw, y_raw - last_y_raw);
                    last_direction_time = current_time;
                    detected_emotion = "Accelerate";
                }
                // 检测后退倾角：Y轴从正数变为较小的正数或负数（但绝对值不大） -> 触发急刹表情
                // 根据数据，后退时Y轴可能回到接近静止状态，或者有特定的变化模式
                // 这里暂时使用Y轴从较大正值快速减小来判断后退
                else if (last_y_raw > y_forward_threshold && y_raw < last_y_raw - 500) {
                    ESP_LOGI("LIS2HH12", "=== 方向检测：后退倾角 ===");
                    ESP_LOGI("LIS2HH12", "检测到后退倾角: Y轴=%d -> %d (变化=%d), 触发表情: Decelerate -> 组15 (急刹表情)", 
                             last_y_raw, y_raw, y_raw - last_y_raw);
                    last_direction_time = current_time;
                    detected_emotion = "Decelerate";
                }
                
                // 如果检测到方向变化，在视频模式下处理
                if (detected_emotion != nullptr) {
                    if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                        // 如果触发的表情和上次相同，且正在显示陀螺仪表情，不重新启动定时器
                        if (board->gyro_emotion_active_ && 
                            board->last_gyro_emotion_ != nullptr && 
                            strcmp(detected_emotion, board->last_gyro_emotion_) == 0) {
                            ESP_LOGI("LIS2HH12", "视频模式下触发相同陀螺仪表情: %s，跳过（已在显示中）", detected_emotion);
                            // 跳过，不重新启动定时器，但继续更新 last_x_raw 和 last_y_raw
                            last_x_raw = x_raw;
                            last_y_raw = y_raw;
                            last_total_accel = total_accel;
                            vTaskDelay(pdMS_TO_TICKS(100));  // 延迟100ms
                            continue;  // 跳过本次循环的后续处理
                        }
                        
                        // 视频模式下：暂停轮播，显示陀螺仪触发的表情，5秒后恢复轮播
                        ESP_LOGI("LIS2HH12", "视频模式下触发陀螺仪表情: %s，暂停轮播，5秒后恢复", detected_emotion);
                        board->gyro_emotion_active_ = true;
                        board->last_gyro_emotion_ = detected_emotion;  // 记录本次触发的表情
                        board->TriggerEmotion(detected_emotion);
                        
                        // 创建或重启5秒恢复定时器
                        if (board->gyro_emotion_timer_ == nullptr) {
                            esp_timer_create_args_t timer_args = {
                                .callback = [](void* arg) {
                                    MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                    ESP_LOGI("LIS2HH12", "陀螺仪表情5秒到期，恢复轮播");
                                    board->ResumeVideoCycling();
                                },
                                .arg = board,
                                .name = "gyro_emotion_timer"
                            };
                            esp_timer_create(&timer_args, &board->gyro_emotion_timer_);
                        }
                        esp_timer_stop(board->gyro_emotion_timer_);  // 先停止（如果正在运行）
                        esp_timer_start_once(board->gyro_emotion_timer_, 5000000);  // 5秒 = 5000000微秒
                    } else {
                        // Display模式下：直接触发表情
                        board->TriggerEmotion(detected_emotion);
                    }
                }
            }
            */
            
            last_x_raw = x_raw;
            last_y_raw = y_raw;
            
            // 检测是否有明显的总加速度变化（比单轴变化更准确）
            // 暂时注释掉，先调通整体逻辑
            /*
            if (delta_total > threshold) {
                shake_count++;
                
                if (shake_count >= shake_count_threshold) {
                    // 检查是否已经过了冷却时间
                    TickType_t current_time = xTaskGetTickCount();
                    if (current_time - last_shake_time >= shake_cooldown) {
                        ESP_LOGI("LIS2HH12", "摇晃检测成功");
                        ESP_LOGI("LIS2HH12", "触发时数据: X=%.3f, Y=%.3f, Z=%.3f, 总加速度=%.3f, 变化量=%.3f", 
                                 ax, ay, az, total_accel, delta_total);
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
                        shake_count = 0; // 重置计数但不触发
                    }
                }
            } else {
                // 没有检测到明显变化，减少计数
                if (shake_count > 0) {
                    shake_count -= shake_count_decay;
                }
            }
            */
            
            last_total_accel = total_accel;
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms采样间隔
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
        
        // 参考 gizwits-s3-vb6824-st7735s 项目，让 LVGL 完成首次界面创建并强制刷新 2~3 帧，确保显示内容完全准备好
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        if (lvgl_port_lock(100)) {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 重写 Board 基类的 PlayVideoGroup 方法，使用 VideoPlayer 类（和 SetEmotion 一样的调用方式）
    void PlayVideoGroup(const char* emotion) override {
        ESP_LOGI(TAG, "=== PlayVideoGroup 开始 ===");
        ESP_LOGI(TAG, "PlayVideoGroup: emotion='%s', video_player_=%p", 
                 emotion ? emotion : "nullptr", video_player_);
        
        if (emotion == nullptr || video_player_ == nullptr) {
            ESP_LOGE(TAG, "PlayVideoGroup: emotion is nullptr or video_player_ is nullptr");
            return;
        }
        // 使用 VideoPlayer 类播放视频
        ESP_LOGI(TAG, "PlayVideoGroup: 调用 video_player_->PlayVideoGroup('%s')", emotion);
        video_player_->PlayVideoGroup(emotion);
        ESP_LOGI(TAG, "PlayVideoGroup: 调用完成");
    }

public:
    // 统一的表情触发函数：统一使用视频播放（默认表情映射也使用视频表情）
    // 所有表情触发（包括AI对话、按钮、传感器等）都会通过这里
    // 使用 VideoPlayer::DefaultEmotionToGroup 映射表来映射表情名称到视频组索引
    void TriggerEmotion(const char* emotion) {
        ESP_LOGI(TAG, "=== TriggerEmotion 开始 ===");
        ESP_LOGI(TAG, "TriggerEmotion: emotion='%s', video_player_=%p, playback_mode_=%d", 
                 emotion ? emotion : "nullptr", video_player_, (int)playback_mode_);
        
        if (emotion == nullptr) {
            ESP_LOGW(TAG, "TriggerEmotion: emotion is nullptr, 返回");
            return;
        }
        
        // 检查是否在配网模式，如果是则忽略表情切换（二维码应该一直显示）
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring || qrcode_displaying_) {
            ESP_LOGI(TAG, "TriggerEmotion: 配网模式或正在显示二维码，忽略表情切换");
            return;
        }
        
        // 统一使用视频播放，两种模式都播放视频，只是播放方式不同
        // VIDEO_PLAYBACK模式：不循环，播放完切换下一组（自动轮播）
        // DISPLAY_ANIMATION模式：循环播放同一组（不轮播）
        if (video_player_ != nullptr) {
            if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                video_player_->SetLoopGroup(false);  // 不循环，播放完切换
                ESP_LOGI(TAG, "TriggerEmotion: 视频模式，调用 PlayVideoGroup('%s')，不循环", emotion);
            } else {
                video_player_->SetLoopGroup(true);   // 循环播放同一组
                ESP_LOGI(TAG, "TriggerEmotion: Display模式，调用 PlayVideoGroup('%s')，循环播放", emotion);
            }
            PlayVideoGroup(emotion);
            
            // 确保视频图像对象显示，眼睛对象（容器）隐藏
            if (display_ != nullptr) {
                if (display_->Lock(100)) {
                    lv_obj_t* screen = lv_screen_active();
                    if (screen != nullptr) {
                        // 获取充电圆环对象指针，在隐藏时排除它
                        EyeDisplay* eye_display = static_cast<EyeDisplay*>(display_);
                        lv_obj_t* charging_arc = (eye_display != nullptr) ? eye_display->GetChargingBatteryArc() : nullptr;
                        
                        // 递归函数：隐藏对象及其所有子对象（用于隐藏眼睛容器）
                        std::function<void(lv_obj_t*)> hide_recursive;
                        hide_recursive = [&hide_recursive, &charging_arc](lv_obj_t* obj) -> void {
                            if (obj == nullptr || obj == charging_arc) return;
                            // 隐藏对象
                            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                            // 递归隐藏所有子对象
                            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
                            for (uint32_t i = 0; i < child_cnt; i++) {
                                lv_obj_t* child = lv_obj_get_child(obj, i);
                                if (child != nullptr) {
                                    hide_recursive(child);
                                }
                            }
                        };
                        
                        // 显示视频图像对象，隐藏眼睛对象（容器）
                        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                        uint32_t hidden_count = 0;
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t* child = lv_obj_get_child(screen, i);
                            if (child != nullptr && child != charging_arc) {
                                if (lv_obj_check_type(child, &lv_image_class)) {
                                    // 视频图像对象：显示且在最前面
                                    lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_move_foreground(child);
                                } else {
                                    // 眼睛对象（容器）：递归隐藏
                                    hide_recursive(child);
                                    hidden_count++;
                                }
                            }
                        }
                        ESP_LOGI(TAG, "TriggerEmotion: 隐藏了 %u 个眼睛对象，视频图像已显示", hidden_count);
                    }
                    display_->Unlock();
                } else {
                    ESP_LOGW(TAG, "TriggerEmotion: 无法获取Display锁");
                }
            }
        } else {
            ESP_LOGE(TAG, "TriggerEmotion: video_player_ 为 nullptr，无法播放视频");
        }
        ESP_LOGI(TAG, "=== TriggerEmotion 完成 ===");
    }

    // 切换播放模式
    void SwitchPlaybackMode() {
        auto& app = Application::GetInstance();
        ESP_LOGI(TAG, "SwitchPlaybackMode: 开始切换，当前模式=%d", (int)playback_mode_);
        
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
            
            // 启动视频轮播（会从组0开始播放）
            StartVideoCycling();
            
            // 视频模式下禁用唤醒词检测和语音处理（离线模式）
            app.GetAudioService().EnableWakeWordDetection(false);
            app.GetAudioService().EnableVoiceProcessing(false);
        } else {
            ESP_LOGI(TAG, "切换到Display动画模式");
            playback_mode_ = PlaybackMode::DISPLAY_ANIMATION;
            
            // 先停止视频轮播并清除回调
            StopVideoCycling();
            
            // 停止当前视频播放，确保清除所有回调
            if (video_player_ != nullptr) {
                video_player_->StopPlayback();
                // 等待更长时间，确保视频任务完全退出并清理句柄
                // 因为视频任务可能在读取Flash或更新LVGL，需要更多时间
                vTaskDelay(pdMS_TO_TICKS(500));
                // 再次检查并强制停止（如果任务还在运行）
                video_player_->StopPlayback();
                vTaskDelay(pdMS_TO_TICKS(100));
                // 再次清除回调，确保没有残留的回调
                video_player_->SetOnGroupFinishedCallback(nullptr, nullptr);
            }
            
            // 参考 gizwits-s3-vb6824-st7735s 项目，不在模式切换时管理音频状态
            // 让 Application::SetDeviceState 自己管理唤醒词检测和语音处理
            ESP_LOGI(TAG, "Display模式：音频状态由 Application::SetDeviceState 管理");
            
            // Display模式也使用视频播放，只是循环播放同一组（不轮播）
            // 确保眼睛对象隐藏，视频图像对象显示
            if (display_ != nullptr) {
                if (display_->Lock(1000)) {
                    lv_obj_t* screen = lv_screen_active();
                    if (screen != nullptr) {
                        // 获取充电圆环对象指针，在隐藏时排除它
                        EyeDisplay* eye_display = static_cast<EyeDisplay*>(display_);
                        lv_obj_t* charging_arc = (eye_display != nullptr) ? eye_display->GetChargingBatteryArc() : nullptr;
                        
                        // 递归函数：隐藏对象及其所有子对象（用于隐藏眼睛容器）
                        std::function<void(lv_obj_t*)> hide_recursive;
                        hide_recursive = [&hide_recursive, &charging_arc](lv_obj_t* obj) -> void {
                            if (obj == nullptr || obj == charging_arc) return;
                            // 隐藏对象
                            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                            // 递归隐藏所有子对象
                            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
                            for (uint32_t i = 0; i < child_cnt; i++) {
                                lv_obj_t* child = lv_obj_get_child(obj, i);
                                if (child != nullptr) {
                                    hide_recursive(child);
                                }
                            }
                        };
                        
                        // 隐藏眼睛对象（容器），显示视频图像对象
                        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                        uint32_t hidden_count = 0;
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t* child = lv_obj_get_child(screen, i);
                            if (child != nullptr && child != charging_arc) {
                                if (lv_obj_check_type(child, &lv_image_class)) {
                                    // 视频图像对象：显示且在最前面
                                    lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_move_foreground(child);
                                } else {
                                    // 眼睛对象（容器）：递归隐藏
                                    hide_recursive(child);
                                    hidden_count++;
                                }
                            }
                        }
                        ESP_LOGI(TAG, "SwitchPlaybackMode: 隐藏了 %u 个眼睛对象，视频图像已显示", hidden_count);
                    }
                    display_->Unlock();
                } else {
                    ESP_LOGW(TAG, "Failed to lock display");
                }
                
                // Display模式也使用视频播放，循环播放同一组
                // 延迟一下，确保之前的视频任务完全退出
                vTaskDelay(pdMS_TO_TICKS(100));
                TriggerEmotion("neutral");
            } else {
                ESP_LOGW(TAG, "Display is nullptr");
            }
            ESP_LOGI(TAG, "SwitchPlaybackMode: 已切换到Display模式并触发neutral表情");
        }
    }

    // 循环切换Display表情
    void CycleDisplayEmotion() {
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
        
        
        // 触发表情切换
        TriggerEmotion(next_emotion);
    }

    void CycleVideoGroup() {
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

        // 获取当前播放的组索引
        // 注意：如果视频任务刚退出，GetCurrentGroupIndex() 可能返回旧值
        // 所以需要确保在任务完全退出后再获取组索引
        int current_group = video_player_->GetCurrentGroupIndex();
        
        // 切换到下一个组（循环）：播完最后一个组后回到组0
        int next_group = (current_group + 1) % cnt;
        
        // 如果当前组索引无效或超出范围，从组0开始
        if (current_group < 0 || current_group >= cnt) {
            ESP_LOGW(TAG, "CycleVideoGroup: 当前组索引无效 (%d)，从组0开始", current_group);
            next_group = 0;
        }
        
        ESP_LOGI(TAG, "CycleVideoGroup: 当前组=%d, 下一组=%d, 总组数=%d", current_group, next_group, cnt);
        
        // 直接使用组索引播放，确保所有组都能循环播放
        video_player_->PlayVideoGroupByIndex(next_group);
    }
    
    // 启动视频轮播（自动切换所有表情）
    void StartVideoCycling() {
        if (playback_mode_ != PlaybackMode::VIDEO_PLAYBACK) {
            ESP_LOGW(TAG, "StartVideoCycling: 不在视频模式，不启动轮播");
            return;
        }
        
        if (video_cycling_) {
            ESP_LOGI(TAG, "StartVideoCycling: 轮播已在进行中");
            return;
        }
        
        ESP_LOGI(TAG, "StartVideoCycling: 启动视频轮播（播完一组自动切换下一组）");
        video_cycling_ = true;
        
        // 不再使用定时器，改为在视频播放完成时自动切换
        // 设置播放完成回调，当一组播放完成时自动切换到下一组
        if (video_player_ != nullptr) {
            video_player_->SetOnGroupFinishedCallback([](void* arg, int group_index) {
                MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                // 只在视频模式下才处理轮播
                if (board->playback_mode_ != PlaybackMode::VIDEO_PLAYBACK) {
                    ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 不在视频模式，跳过切换");
                    return;
                }
                // 如果正在显示陀螺仪触发的表情，不切换
                if (board->gyro_emotion_active_) {
                    ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 正在显示陀螺仪表情，跳过切换");
                    return;
                }
                // 如果轮播已停止，不切换
                if (!board->video_cycling_) {
                    ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 轮播已停止，跳过切换");
                    return;
                }
                // 切换到下一个组
                ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 组 %d 播放完成，切换到下一组", group_index);
                // 先等待一下，确保视频任务完全退出（任务可能在回调后还在清理资源）
                vTaskDelay(pdMS_TO_TICKS(100));
                board->CycleVideoGroup();
            }, this);
        }
        
        // 从组0开始播放
        if (video_player_ != nullptr) {
            // 视频模式下不循环播放同一组，播放完一组后切换下一组
            video_player_->SetLoopGroup(false);
            video_player_->PlayVideoGroupByIndex(0);
        }
    }
    
    // 停止视频轮播
    void StopVideoCycling() {
        if (!video_cycling_) {
            return;
        }
        
        ESP_LOGI(TAG, "StopVideoCycling: 停止视频轮播");
        video_cycling_ = false;
        
        if (video_cycle_timer_ != nullptr) {
            esp_timer_stop(video_cycle_timer_);
        }
        
        // 清除播放完成回调，避免在非视频模式下触发
        if (video_player_ != nullptr) {
            video_player_->SetOnGroupFinishedCallback(nullptr, nullptr);
        }
    }
    
    // 恢复视频轮播（从陀螺仪表情恢复）
    void ResumeVideoCycling() {
        ESP_LOGI(TAG, "ResumeVideoCycling: 恢复视频轮播");
        gyro_emotion_active_ = false;
        last_gyro_emotion_ = nullptr;  // 清除上次触发的表情记录
        
        // 停止陀螺仪表情恢复定时器
        if (gyro_emotion_timer_ != nullptr) {
            esp_timer_stop(gyro_emotion_timer_);
        }
        
        // 如果轮播已启动，继续轮播
        if (video_cycling_) {
            // 轮播定时器会自动继续工作
            ESP_LOGI(TAG, "ResumeVideoCycling: 轮播继续");
        } else {
            // 如果轮播未启动，重新启动
            StartVideoCycling();
        }
    }

    int MaxBacklightBrightness() {
        return 8;
    }

    // 显示电量圆环指示器（委托给EyeDisplay）
    void ShowBatteryIndicator() {
        if (display_ != nullptr) {
            // 无论什么模式，都要停止视频播放（因为所有表情都通过视频播放）
            if (video_player_ != nullptr) {
                int current_group_index = video_player_->GetCurrentGroupIndex();
                // 在停止播放之前保存组索引和轮播状态
                saved_video_group_index_for_battery_ = current_group_index;
                was_video_cycling_before_battery_ = video_cycling_;  // 保存轮播状态
                
                // 停止视频播放和轮播
                video_player_->StopPlayback();
                if (was_video_cycling_before_battery_) {
                    StopVideoCycling();  // 停止轮播
                }
                
                // 等待更长时间，确保视频任务完全退出（因为任务可能在读取Flash或更新LVGL）
                vTaskDelay(pdMS_TO_TICKS(500));
                
                // 再次确认视频任务已停止
                video_player_->StopPlayback();
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // 根据模式设置视频模式信息，以便5秒后恢复
                if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                    display_->SetVideoModeInfo(true, current_group_index);
                } else {
                    // DISPLAY_ANIMATION模式，也保存状态以便恢复
                    display_->SetVideoModeInfo(false, current_group_index);
                }
            } else {
                saved_video_group_index_for_battery_ = -1;
                was_video_cycling_before_battery_ = false;
                display_->SetVideoModeInfo(false, -1);
            }
            display_->ShowBatteryIndicator();
        }
    }
    
    // 隐藏电量圆环指示器（委托给EyeDisplay）
    void HideBatteryIndicator() {
        if (display_ != nullptr) {
            // 使用之前保存的视频组索引和轮播状态
            int saved_video_group_index = saved_video_group_index_for_battery_;
            bool was_cycling = was_video_cycling_before_battery_;
            
            // 检查是否正在充电，如果正在充电，保存状态以便恢复后不重新显示充电圆环
            bool was_charging = IsCharging();
            
            display_->HideBatteryIndicator();
            
            // 无论什么模式，都要恢复视频播放（因为所有表情都通过视频播放）
            if (video_player_ != nullptr && saved_video_group_index >= 0) {
                // 确保之前的视频任务已经完全停止（在ShowBatteryIndicator中已经停止，但再次确认）
                video_player_->StopPlayback();
                // 等待更长时间，确保视频任务完全退出（因为任务可能在读取Flash或更新LVGL）
                vTaskDelay(pdMS_TO_TICKS(500));
                
                // 再次确认视频任务已停止
                video_player_->StopPlayback();
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // 根据模式恢复视频播放
                if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                // 视频模式：如果之前在轮播，恢复轮播；否则只播放一个表情
                if (was_cycling) {
                    ESP_LOGI(TAG, "HideBatteryIndicator: 恢复视频轮播，从组 %d 开始", saved_video_group_index);
                    // 恢复轮播：手动设置轮播状态和回调，然后从保存的组开始播放
                    // 不调用 StartVideoCycling()，因为它会从组0开始
                    video_cycling_ = true;
                    // 设置播放完成回调，当一组播放完成时自动切换到下一组
                    if (video_player_ != nullptr) {
                        video_player_->SetOnGroupFinishedCallback([](void* arg, int group_index) {
                            MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                            // 只在视频模式下才处理轮播
                            if (board->playback_mode_ != PlaybackMode::VIDEO_PLAYBACK) {
                                ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 不在视频模式，跳过切换");
                                return;
                            }
                            // 如果正在显示陀螺仪触发的表情，不切换
                            if (board->gyro_emotion_active_) {
                                ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 正在显示陀螺仪表情，跳过切换");
                                return;
                            }
                            // 如果轮播已停止，不切换
                            if (!board->video_cycling_) {
                                ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 轮播已停止，跳过切换");
                                return;
                            }
                            // 切换到下一个组
                            ESP_LOGI("MovecallMojiESP32S3", "播放完成回调: 组 %d 播放完成，切换到下一组", group_index);
                            board->CycleVideoGroup();
                        }, this);
                        // 视频模式下不循环播放同一组，播放完一组后切换下一组
                        video_player_->SetLoopGroup(false);
                        // 从保存的组开始播放
                        video_player_->PlayVideoGroupByIndex(saved_video_group_index);
                    }
                    } else {
                        // 不在轮播，只播放一个表情
                        static const char* emotion_list[] = {
                            "happy",        // 组 0
                            "neutral",      // 组 1
                            "sad",          // 组 2
                            "surprised",    // 组 3
                            "angry",        // 组 4
                            "loving",       // 组 5
                            "thinking",     // 组 6
                            "winking",      // 组 7
                            "sleepy",       // 组 8
                            "silly",        // 组 9
                            "vertigo",      // 组 10
                            "listen",       // 组 11
                            "Turn_left",    // 组 12
                            "Turn_right",   // 组 13
                            "Accelerate",   // 组 14
                            "Decelerate",   // 组 15
                            "Charging"      // 组 16
                        };
                        const int emotion_list_size = sizeof(emotion_list) / sizeof(emotion_list[0]);
                        
                        if (saved_video_group_index >= 0 && saved_video_group_index < emotion_list_size) {
                            const char* emotion = emotion_list[saved_video_group_index];
                            ESP_LOGI(TAG, "HideBatteryIndicator: 恢复之前的表情 '%s' (视频模式，不轮播)", 
                                     emotion);
                            TriggerEmotion(emotion);
                        } else {
                            ESP_LOGI(TAG, "HideBatteryIndicator: 索引超出范围，使用默认neutral");
                            TriggerEmotion("neutral");
                        }
                    }
                } else {
                    // DISPLAY_ANIMATION模式：只播放一个表情
                    static const char* emotion_list[] = {
                        "happy",        // 组 0
                        "neutral",      // 组 1
                        "sad",          // 组 2
                        "surprised",    // 组 3
                        "angry",        // 组 4
                        "loving",       // 组 5
                        "thinking",     // 组 6
                        "winking",      // 组 7
                        "sleepy",       // 组 8
                        "silly",        // 组 9
                        "vertigo",      // 组 10
                        "listen",       // 组 11
                        "Turn_left",    // 组 12
                        "Turn_right",   // 组 13
                        "Accelerate",   // 组 14
                        "Decelerate",   // 组 15
                        "Charging"      // 组 16
                    };
                    const int emotion_list_size = sizeof(emotion_list) / sizeof(emotion_list[0]);
                    
                    if (saved_video_group_index >= 0 && saved_video_group_index < emotion_list_size) {
                        const char* emotion = emotion_list[saved_video_group_index];
                        ESP_LOGI(TAG, "HideBatteryIndicator: 恢复之前的表情 '%s' (Display模式)", 
                                 emotion);
                        TriggerEmotion(emotion);
                    } else {
                        ESP_LOGI(TAG, "HideBatteryIndicator: 索引超出范围，使用默认neutral");
                        TriggerEmotion("neutral");
                    }
                }
                
                // 如果之前正在充电，恢复表情后确保充电圆环显示（如果被隐藏了）
                // 但不要重新刷新，只确保它显示即可（如果圆环已存在，就不调用 ShowBatteryIndicatorForCharging）
                if (was_charging && display_ != nullptr) {
                    ESP_LOGI(TAG, "HideBatteryIndicator: 正在充电，检查充电圆环状态");
                    auto& app = Application::GetInstance();
                    auto device_state = app.GetDeviceState();
                    if (device_state != kDeviceStateWifiConfiguring) {
                        // 检查充电圆环是否已经存在（通过 EyeDisplay 的 GetChargingBatteryArc 方法）
                        EyeDisplay* eye_display = static_cast<EyeDisplay*>(display_);
                        lv_obj_t* charging_arc = (eye_display != nullptr) ? eye_display->GetChargingBatteryArc() : nullptr;
                        if (charging_arc != nullptr && lv_obj_is_valid(charging_arc)) {
                            // 充电圆环已存在，只确保它显示（如果被隐藏了）
                            ESP_LOGI(TAG, "HideBatteryIndicator: 充电圆环已存在，只确保显示，不重新刷新");
                            if (display_->Lock(100)) {
                                if (lv_obj_has_flag(charging_arc, LV_OBJ_FLAG_HIDDEN)) {
                                    lv_obj_clear_flag(charging_arc, LV_OBJ_FLAG_HIDDEN);
                                    ESP_LOGI(TAG, "HideBatteryIndicator: 充电圆环被隐藏，恢复显示");
                                }
                                // 确保充电圆环在最前面
                                lv_obj_move_foreground(charging_arc);
                                display_->Unlock();
                            }
                        } else {
                            // 充电圆环不存在，需要创建（这种情况应该很少，因为充电时圆环应该一直显示）
                            ESP_LOGI(TAG, "HideBatteryIndicator: 充电圆环不存在，创建新的圆环");
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
                }
                
                // 清除保存的索引和轮播状态
                saved_video_group_index_for_battery_ = -1;
                was_video_cycling_before_battery_ = false;
            } else {
                ESP_LOGW(TAG, "HideBatteryIndicator: 不恢复视频播放 - video_player_=%p, saved_video_group_index=%d", 
                         video_player_, saved_video_group_index);
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

        // 触摸按钮功能已禁用（注释掉）
        /*
        touch_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "=== 触摸按钮检测到按下 ===");
            ESP_LOGI(TAG, "touch_button_.OnPressDown");

            TickType_t current_time = xTaskGetTickCount();
            const TickType_t touch_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间
            TickType_t time_since_last_touch = current_time - last_touch_time_;
            TickType_t remaining_cooldown = (time_since_last_touch < touch_cooldown) ? 
                                            (touch_cooldown - time_since_last_touch) : 0;
            
            ESP_LOGI(TAG, "触摸时间: %lu ms, 上次触摸: %lu ms, 距离上次: %lu ms, 冷却剩余: %lu ms", 
                     current_time * portTICK_PERIOD_MS, 
                     last_touch_time_ * portTICK_PERIOD_MS,
                     time_since_last_touch * portTICK_PERIOD_MS,
                     remaining_cooldown * portTICK_PERIOD_MS);
            
            // 检查是否已经过了冷却时间
            if (current_time - last_touch_time_ >= touch_cooldown) {
                last_touch_time_ = current_time; // 更新上次触发时间
                ESP_LOGI(TAG, "✓ 冷却时间已过，处理触摸事件");

                //切换表情
                if (CheckAndHandleEnterSleepMode()) {
                    // 交给休眠逻辑托管
                    ESP_LOGI(TAG, "触摸唤醒 - 设备从休眠中唤醒");
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
                        ESP_LOGI(TAG, "Channel未打开，切换聊天状态");
                        Application::GetInstance().ToggleChatState();
                    }
                }
                ESP_LOGI(TAG, "=== 触摸事件处理完成 ===");
            } else {
                ESP_LOGI(TAG, "⚠ 触摸检测到但仍在冷却期，剩余 %lu ms，忽略本次触摸", 
                         remaining_cooldown * portTICK_PERIOD_MS);
            }
        });
        */

        // 创建双击检测定时器（使用esp_timer，避免栈溢出）
        if (!boot_button_timer_) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                    // 定时器超时，执行单击操作
                    // 注意：双击、三击、四击都在 OnPressRepeaDone 中立即处理，不会到达这里
                    if (board->boot_button_click_count_ == 1) {
                        // 单击：检查是否在显示二维码状态
                        if (board->qrcode_displaying_) {
                            // 如果正在显示二维码，根据当前模式决定操作
                            if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                                // 视频模式下，取消二维码并切换回Display模式
                                board->ExitQrcodeAndEnterDisplayMode();
                            } else {
                                // Display模式下，取消二维码并切换到视频模式
                                board->ExitQrcodeAndEnterVideoMode();
                            }
                        } else if (board->playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                            // 视频模式下，按键切换已禁用（使用自动轮播）
                            ESP_LOGI("MovecallMojiESP32S3", "视频模式下按键切换已禁用，使用自动轮播");
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
                    }
                    // 重置计数器
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

        // 所有多击操作都在 OnPressRepeaDone 中处理，实现立即响应，不需要等待定时器超时
        boot_button_.OnPressRepeaDone([this](uint16_t repeat_count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeaDone - 重复次数: %d", repeat_count);
            
            // 根据重复次数立即执行对应操作，提高响应速度
            if (repeat_count == 2) {
                // 双击：显示电量
                ESP_LOGI(TAG, "双击检测 - 显示电量");
                ShowBatteryIndicator();
            } else if (repeat_count == 3) {
                // 三击：进入配网模式
                ESP_LOGI(TAG, "三击检测 - 直接进入配网模式");
                InnerResetWifiConfiguration();
            } else if (repeat_count >= 4) {
                // 四击：切换模式
                ESP_LOGI(TAG, "四击检测 - 直接切换模式，当前模式=%d", (int)playback_mode_);
                SwitchPlaybackMode();
                ESP_LOGI(TAG, "四击检测 - 切换模式完成，新模式=%d", (int)playback_mode_);
            }
            // 注意：单击（repeat_count == 1）仍然在定时器回调中处理，需要等待确认没有第二次点击
        });
        
        // OnClick 只处理单击的情况（需要等待确认没有第二次点击）
        // 双击、三击、四击都在 OnPressRepeaDone 中立即处理，提高响应速度
        boot_button_.OnClick([this]() {
            // 视频模式下禁用按键切换表情
            if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
                ESP_LOGI(TAG, "视频模式下按键切换已禁用，使用自动轮播");
                return;
            }
            
            int64_t now_ms = esp_timer_get_time() / 1000;
            const int64_t DOUBLE_CLICK_WINDOW_MS = kDoubleClickWindowMs;  // 使用成员变量的双击窗口时间（800ms）
            
            // 如果距离上次点击超过双击窗口，重置计数器
            if (now_ms - boot_button_last_click_ms_ > DOUBLE_CLICK_WINDOW_MS) {
                boot_button_click_count_ = 0;
                ESP_LOGI(TAG, "boot_button_.OnClick - 重置计数器（超过时间窗口）");
            }
            boot_button_last_click_ms_ = now_ms;
            boot_button_click_count_++;
            
            // 停止之前的定时器（如果有）
            if (boot_button_timer_ != nullptr) {
                esp_timer_stop(boot_button_timer_);
            }
            
            // 只有第一次点击时启动定时器，等待确认没有第二次点击后执行单击操作
            // 双击、三击、四击都在 OnPressRepeaDone 中立即处理，不会到达这里
            if (boot_button_click_count_ == 1) {
                ESP_LOGI(TAG, "第一次点击，启动定时器 %d ms 后执行单击操作", DOUBLE_CLICK_WINDOW_MS);
                esp_timer_start_once(boot_button_timer_, DOUBLE_CLICK_WINDOW_MS * 1000);
            } else {
                // 如果检测到第二次或更多次点击，说明是多击操作，停止定时器
                // 多击操作会在 OnPressRepeaDone 中处理
                ESP_LOGI(TAG, "检测到多击操作（点击次数: %d），停止定时器，等待 OnPressRepeaDone 处理", boot_button_click_count_);
                esp_timer_stop(boot_button_timer_);
                boot_button_click_count_ = 0;  // 重置计数器，因为多击操作已经在 OnPressRepeaDone 中处理
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
        
        // 立即停止所有模式的视频播放和轮播，避免访问已删除的对象
        if (video_player_ != nullptr) {
            ESP_LOGI(TAG, "ShowQrcode: 立即停止视频播放和轮播（所有模式）");
            // 如果正在轮播，先停止轮播
            if (video_cycling_) {
                StopVideoCycling();
            }
            // 立即停止视频播放，不等待
            video_player_->StopPlayback();
            // 等待更长时间，确保视频任务完全退出（因为任务可能在读取Flash或更新LVGL）
            vTaskDelay(pdMS_TO_TICKS(500));
            // 再次确认视频任务已停止
            video_player_->StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(200));
            // 第三次确认，确保任务完全退出
            video_player_->StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "ShowQrcode: 视频播放已完全停止");
        }
        
        if (display_ != nullptr) {
            display_->EnterWifiConfig();
            qrcode_displaying_ = true;
            ESP_LOGI(TAG, "ShowQrcode: 二维码已显示，qrcode_displaying_=true");
        }
    }
    
    // 取消二维码并切换回Display模式
    void ExitQrcodeAndEnterDisplayMode() {
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
            
            // 停止视频轮播并清除回调
            StopVideoCycling();
            
            // Display模式下恢复唤醒词检测（根据设备状态）
            auto& app = Application::GetInstance();
            auto device_state = app.GetDeviceState();
            
            // 延迟一下，确保所有状态更新完成后再恢复唤醒词检测
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // 重新获取设备状态（可能已经改变）
            device_state = app.GetDeviceState();
            
            // 如果设备状态是idle或sleeping，恢复唤醒词检测
            if (device_state == kDeviceStateIdle || device_state == kDeviceStateSleeping) {
                app.GetAudioService().EnableWakeWordDetection(true);
                app.GetAudioService().EnableVoiceProcessing(false);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（idle/sleeping状态）");
            } else if (device_state == kDeviceStateSpeaking) {
                // 只在speaking状态下启用唤醒词检测（用于打断）
                #if CONFIG_USE_AFE_WAKE_WORD
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（speaking状态，AFE唤醒词，用于打断）");
                #else
                // 非AFE唤醒词，在speaking状态下不启用唤醒词检测
                ESP_LOGW(TAG, "Display模式：speaking状态下非AFE唤醒词不启用唤醒词检测");
                #endif
            } else if (device_state == kDeviceStateListening) {
                // listening状态下不启用唤醒词检测，避免与语音处理冲突
                ESP_LOGI(TAG, "Display模式：listening状态下不启用唤醒词检测（避免与语音处理冲突）");
            } else {
                // 其他状态，也尝试启用唤醒词检测（如果设备允许）
                app.GetAudioService().EnableWakeWordDetection(true);
                ESP_LOGI(TAG, "Display模式：已恢复唤醒词检测（其他状态）");
            }
            
            // 再次等待，确保所有操作完成和屏幕稳定
            vTaskDelay(pdMS_TO_TICKS(200));
            
            // 统一使用视频播放，重新初始化表情（屏幕已经清空）
            TriggerEmotion("neutral");
        }
    }
    
    // 取消二维码并进入视频模式
    void ExitQrcodeAndEnterVideoMode() {
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
            
            // 检查充电状态，如果正在充电，显示电量圆环（因为状态改变回调可能不会触发）
            if (IsCharging()) {
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
            
            // 启动视频轮播（会从组0开始播放）
            StartVideoCycling();
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
        
        // 自动检测设备地址（尝试 0x1D 和 0x1E）
        uint8_t detected_addr = 0;
        uint8_t addresses[] = {0x1D, 0x1E};  // SDO接GND为0x1D，接VDD为0x1E
        
        for (int i = 0; i < 2; i++) {
            uint8_t addr = addresses[i];
            ESP_LOGI(TAG, "尝试检测 LIS2HH12 地址: 0x%02X", addr);
            
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100000,  // 使用100kHz标准速度
            };
            
            i2c_master_dev_handle_t test_dev = nullptr;
            ret = i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &test_dev);
            if (ret == ESP_OK && test_dev != nullptr) {
                // 尝试读取 WHO_AM_I 寄存器验证
                uint8_t who_am_i_reg = 0x0F;
                uint8_t who_am_i_val = 0;
                ret = i2c_master_transmit_receive(test_dev, &who_am_i_reg, 1, &who_am_i_val, 1, pdMS_TO_TICKS(500));
                
                if (ret == ESP_OK && who_am_i_val == 0x41) {
                    // 找到正确的设备地址
                    detected_addr = addr;
                    lis2hh12_dev_ = test_dev;  // 使用这个设备句柄
                    ESP_LOGI(TAG, "✓ LIS2HH12 检测成功！地址: 0x%02X, WHO_AM_I: 0x%02X", addr, who_am_i_val);
                    break;
                } else {
                    // 不是 LIS2HH12，删除设备句柄
                    i2c_master_bus_rm_device(test_dev);
                    ESP_LOGD(TAG, "地址 0x%02X 不是 LIS2HH12 (WHO_AM_I: 0x%02X)", addr, who_am_i_val);
                }
            } else {
                ESP_LOGD(TAG, "地址 0x%02X 无法创建设备: %s", addr, esp_err_to_name(ret));
            }
        }
        
        if (detected_addr == 0) {
            ESP_LOGW(TAG, "⚠ 未找到 LIS2HH12 设备，尝试使用默认地址 0x%02X", LIS2HH12_I2C_ADDR);
            // 使用默认地址
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = LIS2HH12_I2C_ADDR,
                .scl_speed_hz = 100000,
            };
            ret = i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &lis2hh12_dev_);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add LIS2HH12 device with default address: %s", esp_err_to_name(ret));
                return;
            }
            ESP_LOGI(TAG, "LIS2HH12 I2C initialized with default address 0x%02X (未验证)", LIS2HH12_I2C_ADDR);
        } else {
            ESP_LOGI(TAG, "LIS2HH12 I2C initialized successfully with auto-detected address 0x%02X", detected_addr);
        }
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
            ESP_LOGE(TAG, "LIS2HH12 read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
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
                // 但如果 GetDisplay() 还没被调用（display_wrapper_ 为 nullptr），不立即显示电量圆环
                // 因为 GetDisplay() 中会先显示表情，然后再显示电量圆环，确保表情先显示
                if (device_state != kDeviceStateWifiConfiguring) {
                    // 如果 GetDisplay() 已经被调用（display_wrapper_ 已创建），立即显示电量圆环
                    // 否则，等待 GetDisplay() 时再显示（GetDisplay() 中会检查充电状态并显示）
                    if (display_wrapper_ != nullptr) {
                        ESP_LOGI(TAG, "显示充电电量圆环（GetDisplay已调用）");
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
                    } else {
                        ESP_LOGI(TAG, "GetDisplay() 还未调用，等待 GetDisplay() 时再显示电量圆环");
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
                
                // 参考 gizwits-s3-vb6824-st7735s 项目，不在状态回调中管理音频状态和视频播放
                // 让 Application::SetDeviceState 自己管理唤醒词检测和语音处理
                // 视频播放任务使用非阻塞锁，不会阻塞音频处理，所以不需要在 listening 状态下停止
                
                // 配网模式：必须停止视频播放和轮播，确保二维码能正常显示
                if (curr == kDeviceStateWifiConfiguring) {
                    ESP_LOGI(TAG, "DeviceStateEventManager回调: 进入配网模式，停止视频播放和轮播");
                    // 如果正在轮播，先停止轮播
                    if (video_cycling_) {
                        StopVideoCycling();
                    }
                    // 停止视频播放
                    if (video_player_ != nullptr) {
                        video_player_->StopPlayback();
                        // 等待视频任务完全退出
                        vTaskDelay(pdMS_TO_TICKS(500));
                        // 再次确认视频任务已停止
                        video_player_->StopPlayback();
                        vTaskDelay(pdMS_TO_TICKS(200));
                        ESP_LOGI(TAG, "DeviceStateEventManager回调: 视频播放已停止");
                    }
                }
                
                // 如果正在充电，根据新状态决定是否显示电量圆环
                bool is_charging = IsCharging();
                ESP_LOGI(TAG, "DeviceStateEventManager回调: is_charging=%d (强制刷新后)", is_charging);
                if (is_charging) {
                    // 只排除配网模式，其他所有状态都显示电量圆环
                    // 但如果 GetDisplay() 还没被调用（display_wrapper_ 为 nullptr），不立即显示电量圆环
                    // 因为 GetDisplay() 中会先显示表情，然后再显示电量圆环，确保表情先显示
                    if (curr != kDeviceStateWifiConfiguring) {
                        // 如果 GetDisplay() 已经被调用（display_wrapper_ 已创建），立即显示电量圆环
                        // 否则，等待 GetDisplay() 时再显示（GetDisplay() 中会检查充电状态并显示）
                        if (display_wrapper_ != nullptr) {
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
                            ESP_LOGI(TAG, "DeviceStateEventManager回调: GetDisplay() 还未调用，等待 GetDisplay() 时再显示电量圆环");
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
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO) {  // 触摸按钮已启用 
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
        xTaskCreatePinnedToCore(MovecallMojiESP32S3::lis2hh12_task, "lis2hh12_task", 1024 * 3, this, 1, NULL, 0); // 启动陀螺仪检测任务
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
            
            // 检查充电状态，但不在这里显示电量圆环
            // 电量圆环会在 GetDisplay() 中，在表情显示之后显示，确保表情先显示
            if (IsCharging()) {
                ESP_LOGI(TAG, "启动时检测到正在充电，将在 GetDisplay() 后显示电量圆环");
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
        // 关机前先停止视频播放，避免视频任务阻塞关机
        if (video_player_ != nullptr) {
            ESP_LOGI(TAG, "关机前，停止视频播放");
            video_player_->StopPlayback();
            // 等待视频任务完全退出
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        gpio_set_level(POWER_GPIO, 0);
    }

    virtual void WakeWordDetected() override {
        // 视频模式下禁用唤醒词检测和AI对话
        if (playback_mode_ == PlaybackMode::VIDEO_PLAYBACK) {
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
        
        // 参考 gizwits-s3-vb6824-st7735s 项目，确保显示内容已经完全渲染
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 恢复背光（表情已经在 GetDisplay 时触发了）
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
            // 参考 gizwits-s3-vb6824-st7735s 项目，立即触发默认表情，不等待背光恢复
            // 显示内容已经在 InitializeGc9a01Display 中通过 lv_timer_handler 准备好了
            if (video_player_ != nullptr) {
                ESP_LOGI(TAG, "GetDisplay: 首次创建DisplayWrapper，立即触发默认neutral视频表情");
                TriggerEmotion("neutral");
                
                // 表情显示后，检查是否需要显示电量圆环（确保表情先显示）
                if (IsCharging()) {
                    ESP_LOGI(TAG, "GetDisplay: 检测到正在充电，在表情显示后显示电量圆环");
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
                        ESP_LOGI(TAG, "GetDisplay: 电量圆环显示完成");
                    }
                }
            } else {
                ESP_LOGW(TAG, "GetDisplay: video_player_ 尚未初始化，跳过默认表情触发");
            }
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
// 所有通过 display->SetEmotion() 的调用（包括AI对话）都会路由到这里
// 然后通过 TriggerEmotion -> PlayVideoGroup -> video_player_->PlayVideoGroup
// 最终使用 VideoPlayer::DefaultEmotionToGroup 映射表来映射表情到视频组
// 注意：不再调用 wrapped_display_->SetEmotion，避免启动Display动画，统一使用视频播放
void DisplayWrapper::SetEmotion(const char* emotion) {
    ESP_LOGI("DisplayWrapper", "=== DisplayWrapper::SetEmotion 开始 ===");
    ESP_LOGI("DisplayWrapper", "SetEmotion: emotion='%s', board_=%p, wrapped_display_=%p", 
             emotion ? emotion : "nullptr", board_, wrapped_display_);
    
    if (board_ != nullptr) {
        // 路由到 TriggerEmotion，统一使用视频播放和 video_player 的映射表
        // 不再调用 wrapped_display_->SetEmotion，避免启动Display动画
        ESP_LOGI("DisplayWrapper", "SetEmotion: 路由到 board_->TriggerEmotion('%s')", emotion);
        board_->TriggerEmotion(emotion);
    } else {
        // 如果board_为nullptr，才回退到wrapped_display_（这种情况不应该发生）
        ESP_LOGW("DisplayWrapper", "SetEmotion: board_ 为 nullptr，回退到 wrapped_display_");
        if (wrapped_display_ != nullptr) {
            ESP_LOGI("DisplayWrapper", "SetEmotion: 路由到 wrapped_display_->SetEmotion('%s')", emotion);
            wrapped_display_->SetEmotion(emotion);
        } else {
            ESP_LOGW("DisplayWrapper", "SetEmotion: board_ 和 wrapped_display_ 都为 nullptr");
        }
    }
    ESP_LOGI("DisplayWrapper", "SetEmotion: 完成");
}

DECLARE_BOARD(MovecallMojiESP32S3);