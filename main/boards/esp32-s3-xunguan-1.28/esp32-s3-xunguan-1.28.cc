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
#include "w25q64_flash.h"

#include "led/single_led.h"
// #include "xunguan_display.h"
#include "display/eye_display.h"
#include "display/display.h"
#include "device_state_event.h"

#include <wifi_station.h>
#include "power_save_timer.h"
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
#include <string.h>
#include <string>

#define TAG "MovecallMojiESP32S3"

// 电源管理定时器配置（单位：秒）
#define POWER_SAVE_SLEEP_SECONDS 60*20        // 第一个定时器：30秒后进入睡眠模式
#define POWER_SAVE_SHUTDOWN_SECONDS 50     // 第二个定时器：50秒后关机（注意：进入睡眠模式后此定时器不会触发，实际由轮播模式睡眠计时定时器替代）
#define VIDEO_CYCLING_SLEEP_SHUTDOWN_SECONDS 60*10  // 轮播模式睡眠计时：20秒后关机（第一个定时器触发后20秒，总共50秒）

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

#define LIS2HH12_I2C_ADDR 0x1D  // SDO接GND为0x1D，接VDD为0x1E
#define LIS2HH12_INT1_PIN GPIO_NUM_42

class MovecallMojiESP32S3 : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    EyeDisplay* display_;
    bool need_power_off_ = false;
    bool sleep_with_video_cycling_ = false;  // 标志：是否在轮播模式下进入睡眠
    bool is_second_timer_sleep_ = false;  // 标志：是否是第二个定时器触发的睡眠（用于显示充电动画）
    i2c_master_bus_handle_t i2c_bus_;
    // LIS2HH12专用I2C
    i2c_master_bus_handle_t lis2hh12_i2c_bus_;
    i2c_master_dev_handle_t lis2hh12_dev_;
    uint8_t lis2hh12_i2c_addr_ = 0x1D;  // 检测到的I2C地址，默认为0x1D
    int64_t power_on_time_ = 0;  // 记录上电时间
    PowerManager* power_manager_;
    TickType_t last_touch_time_ = 0;  // 上次抚摸触发时间
    PowerSaveTimer* power_save_timer_;
    bool is_charging_sleep_ = false;
    
    // 轮播模式下睡眠计时的自定义定时器
    esp_timer_handle_t video_cycling_sleep_timer_ = nullptr;
    int video_cycling_sleep_ticks_ = 0;
    
    // 陀螺仪表情恢复定时器（固定表情模式下使用）
    esp_timer_handle_t gyro_emotion_restore_timer_ = nullptr;
    std::string saved_emotion_before_gyro_;  // 陀螺仪触发前保存的表情
    bool gyro_emotion_active_ = false;  // 是否正在显示陀螺仪触发的表情
    TickType_t last_direction_time_ = 0;  // 上次方向触发时间（用于重置冷却时间）
    
    // 操作检测标志（用于轮播模式下的定时器）
    bool has_user_interaction_ = false;  // 是否有用户操作（陀螺仪或按键）
    TickType_t last_interaction_time_ = 0;  // 上次操作时间

    std::vector<TestItem> test_items = {
        {"lcd", "LCD测试", 1},
        {"key", "按键测试", 0},
        {"wifi", "WiFi连接测试", 0},
        {"sensor", "陀螺仪测试", 0},
        {"battery", "电池检测", 0},
        {"mic", "麦克风检测", 0},
    };


    void InitializePowerSaveTimer() {
        // 使用宏定义配置定时器时间
        power_save_timer_ = new PowerSaveTimer(-1, POWER_SAVE_SLEEP_SECONDS, POWER_SAVE_SHUTDOWN_SECONDS);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "第一个定时器触发，进入睡眠模式（轮播模式）");
            // 第一个定时器：进入睡眠时切换到轮播模式，不调用 SetEmotion
            // 如果已经在轮播模式，直接进入睡眠
            if(display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING){
                ESP_LOGI(TAG, "进入睡眠模式，自动切换到轮播模式");
                Application::GetInstance().QuitTalking();
                display_->ToggleVideoCyclingMode();
            }
            // 设置标志，表示应该在轮播模式下进入睡眠
            sleep_with_video_cycling_ = true;
            // 重置操作标志，进入睡眠后开始检测操作
            has_user_interaction_ = false;
            last_interaction_time_ = xTaskGetTickCount();
            // 注意：音频播放由 Application::EnterSleepMode 统一处理，这里不重复播放
            Application::GetInstance().EnterSleepMode();
            
            // 板级逻辑：延迟启动自定义定时器，确保 EnterSleepMode 完成后再检查轮播模式
            // 因为 EnterSleepMode 是异步的，需要等待状态稳定后再启动定时器
            Application::GetInstance().Schedule([this]() {
                // 等待 EnterSleepMode 完成，确保设备状态稳定
                vTaskDelay(pdMS_TO_TICKS(500));
                // 检查是否在轮播模式，如果是则启动自定义定时器继续计时
                if (display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                    ESP_LOGI(TAG, "第一个定时器触发后，确认在轮播模式，启动轮播模式睡眠计时定时器");
                    StartVideoCyclingSleepTimer();
                } else {
                    ESP_LOGW(TAG, "第一个定时器触发后，不在轮播模式，无法启动轮播模式睡眠计时定时器");
                }
            }, "start_video_cycling_sleep_timer");
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGE(TAG, "退出休眠模式");
            // 板级逻辑：退出睡眠模式时停止轮播模式计时定时器
            StopVideoCyclingSleepTimer();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            // 第二个定时器：根据充电状态决定行为
            // 如果轮播模式下没有操作，才执行关机或显示睡眠动画
            
            // 板级逻辑：检查是否在轮播模式下，如果是则继续计时而不是关机
            bool in_video_cycling = (display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING);
            if (in_video_cycling && power_save_timer_->IsInSleepMode()) {
                // 在轮播模式下，检查是否有用户操作
                if (has_user_interaction_) {
                    ESP_LOGI(TAG, "轮播模式下检测到用户操作，重置定时器");
                    power_save_timer_->ResetTimer();
                    has_user_interaction_ = false;
                } else {
                    // 没有操作，继续计时（不执行关机，等待下次检查）
                    ESP_LOGI(TAG, "轮播模式下继续计时，等待关机条件");
                    // 重要：即使在睡眠模式下也要重置定时器，防止超时
                    power_save_timer_->ResetTimer();
                }
                return;  // 轮播模式下不执行关机逻辑
            }
            
            // 非轮播模式或未进入睡眠模式，执行原有的关机逻辑
            if (!has_user_interaction_) {
                // 检查是否在充电
                bool is_charging = IsCharging();
                if (is_charging) {
                    // 充电时：显示充电动画（不轮播），进入睡眠
                    ESP_LOGI(TAG, "第二个定时器触发，充电中，进入睡眠模式（显示充电动画）");
                    // 设置标志，表示是第二个定时器触发的睡眠
                    is_second_timer_sleep_ = true;
                    Application::GetInstance().QuitTalking();
                    // 确保不是轮播模式，显示充电动画
                    if(display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING){
                        display_->ToggleVideoCyclingMode();  // 退出轮播模式
                    }
                    display_->SetEmotion("Charging");  // 显示充电动画
                    Application::GetInstance().EnterSleepMode();
                } else {
                    // 未充电时：播放关机音频后直接关机
                    ESP_LOGI(TAG, "第二个定时器触发，未充电，准备播放关机音频");
                    auto codec = GetAudioCodec();
                    codec->EnableOutput(true);
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                    // 等待音频播放完成
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    ESP_LOGI(TAG, "第二个定时器触发，未充电，直接关机");
                    PowerOff();
                }
            } else {
                // 有操作，重置定时器
                ESP_LOGI(TAG, "第二个定时器触发，但检测到用户操作，重置定时器");
                power_save_timer_->ResetTimer();
                has_user_interaction_ = false;
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    virtual void ResetPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->ResetTimer();
        }
        // 板级逻辑：重置轮播模式睡眠定时器
        video_cycling_sleep_ticks_ = 0;
    };

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(true);
            power_save_timer_->WakeUp();
        }
        // 板级逻辑：唤醒时停止轮播模式睡眠定时器
        StopVideoCyclingSleepTimer();
    };
    
    // 板级逻辑：启动轮播模式下的睡眠计时定时器
    void StartVideoCyclingSleepTimer() {
        if (video_cycling_sleep_timer_ != nullptr) {
            return;  // 已经启动
        }
        
        video_cycling_sleep_ticks_ = 0;
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<MovecallMojiESP32S3*>(arg);
                self->VideoCyclingSleepTimerCallback();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "video_cycling_sleep",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &video_cycling_sleep_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(video_cycling_sleep_timer_, 1000000));  // 每秒触发一次
        ESP_LOGI(TAG, "轮播模式睡眠计时定时器已启动");
    }
    
    // 板级逻辑：停止轮播模式下的睡眠计时定时器
    void StopVideoCyclingSleepTimer() {
        if (video_cycling_sleep_timer_ != nullptr) {
            esp_timer_stop(video_cycling_sleep_timer_);
            esp_timer_delete(video_cycling_sleep_timer_);
            video_cycling_sleep_timer_ = nullptr;
            video_cycling_sleep_ticks_ = 0;
            ESP_LOGI(TAG, "轮播模式睡眠计时定时器已停止");
        }
    }
    
    // 板级逻辑：轮播模式睡眠计时定时器回调
    void VideoCyclingSleepTimerCallback() {
        // 检查是否仍在轮播模式和睡眠模式
        if (display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING || 
            !power_save_timer_->IsInSleepMode()) {
            // 不在轮播模式或已退出睡眠模式，停止定时器
            StopVideoCyclingSleepTimer();
            return;
        }
        
        // 检查是否有用户操作
        if (has_user_interaction_) {
            ESP_LOGI(TAG, "轮播模式下检测到用户操作，重置定时器");
            video_cycling_sleep_ticks_ = 0;
            has_user_interaction_ = false;
            return;
        }
        
        // 继续计时
        video_cycling_sleep_ticks_++;
        
            // 检查是否满足关机条件
            if (video_cycling_sleep_ticks_ >= VIDEO_CYCLING_SLEEP_SHUTDOWN_SECONDS) {
                ESP_LOGI(TAG, "轮播模式睡眠计时达到关机条件，执行关机");
                StopVideoCyclingSleepTimer();
                
                // 调用原有的关机逻辑
                if (IsCharging()) {
                    // 充电时：显示充电动画（不轮播），保持睡眠状态
                    ESP_LOGI(TAG, "轮播模式睡眠计时触发，充电中，显示充电动画");
                    // 设置标志，表示是第二个定时器触发的睡眠
                    is_second_timer_sleep_ = true;
                    Application::GetInstance().QuitTalking();
                    if(display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING){
                        display_->ToggleVideoCyclingMode();  // 退出轮播模式
                    }
                    display_->SetEmotion("Charging");  // 显示充电动画
                    // 注意：设备已经在睡眠模式了（第一个定时器已触发），不需要再调用 EnterSleepMode()
                    // 但是需要播放音频提醒用户
                    ESP_LOGI(TAG, "轮播模式睡眠计时触发，充电中，播放音频提醒");
                    auto codec = GetAudioCodec();
                    codec->EnableOutput(true);
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                    // 重要：充电状态下不应该继续运行轮播定时器，已经停止
                    return;
                } else {
                    // 未充电时：播放关机音频后直接关机
                    ESP_LOGI(TAG, "轮播模式睡眠计时触发，未充电，准备播放关机音频");
                    auto codec = GetAudioCodec();
                    codec->EnableOutput(true);
                    Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                    // 等待音频播放完成
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    ESP_LOGI(TAG, "轮播模式睡眠计时触发，未充电，直接关机");
                    PowerOff();
                }
            }
    }


    static void lis2hh12_task(void* arg) {
        MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
        float last_total_accel = 0.0f;
        const float threshold = 0.45f; // g-force (摇晃检测阈值)
        int shake_count = 0;
        const int shake_count_threshold = 3; // 连续3次检测到变化才触发摇晃
        const int shake_count_decay = 1;     // 每次没检测到就-1
        TickType_t last_shake_time = 0;      // 上次摇晃触发时间
        const TickType_t shake_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间
        
        // 方向检测阈值（使用变化量检测，类似眩晕检测）
        // 使用raw值的变化量，避免静止状态误触发
        const int16_t x_turn_threshold = 900;   // X轴变化量超过此值判断为转向（与前后阈值一致）
        const int16_t y_forward_threshold = 900;  // Y轴变化量超过此值且为正向变化判断为前进
        // const int16_t y_backward_threshold = 900; // Y轴变化量超过此值且为负向变化判断为后退（暂未使用）
        
        // 方向检测状态
        int16_t last_x_raw = 0;  // 初始化为0，第一次读取后会更新
        int16_t last_y_raw = 0;  // 初始化为0，第一次读取后会更新
        bool first_reading = true;  // 标记是否为第一次读取
        const TickType_t direction_cooldown = pdMS_TO_TICKS(5000); // 方向检测冷却时间5秒
        
        // 方向检测计数（类似摇晃检测）
        int direction_count = 0;
        const int direction_count_threshold = 3; // 连续3次检测到变化才触发
        const int direction_count_decay = 1;     // 每次没检测到就-1
        const char* last_detected_direction = nullptr; // 上次检测到的方向，用于判断方向是否改变
        
        while (1) {
            // 检查设备是否已初始化
            if (!board->is_lis2hh12_initialized()) {
                // 设备未初始化，等待一段时间后重试（可能初始化还在进行中）
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
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
            
            // 方向检测：根据轴的变化量判断方向（类似眩晕检测，使用变化量而不是绝对值）
            TickType_t current_time = xTaskGetTickCount();
            const char* detected_emotion = nullptr;
            const char* direction = nullptr;
            
            // 第一次读取时，初始化last值，不进行方向检测
            if (first_reading) {
                last_x_raw = x_raw;
                last_y_raw = y_raw;
                first_reading = false;
            } else {
                // 计算各轴的变化量（相对于上次的值）
                int16_t x_delta = abs(x_raw - last_x_raw);  // X轴变化量
                int16_t y_delta = abs(y_raw - last_y_raw);  // Y轴变化量
                int16_t y_change = y_raw - last_y_raw;      // Y轴变化方向（正负）
                int16_t x_change = x_raw - last_x_raw;      // X轴变化方向（正负）
                
                // 同等优先级：根据变化量大小判断方向
                // 如果Y轴变化量大于X轴变化量，且达到阈值，判断为前后
                if (y_delta > x_delta && y_delta > y_forward_threshold) {
                    // Y轴减小（向前倾斜）→ 前进
                    if (y_change < 0) {
                        direction = "前进";
                        detected_emotion = "Accelerate";
                    }
                    // Y轴增大（向后倾斜）→ 后退
                    else if (y_change > 0) {
                        direction = "后退";
                        detected_emotion = "Decelerate";
                    }
                }
                // 如果X轴变化量大于Y轴变化量，且达到阈值，判断为左右转
                else if (x_delta > y_delta && x_delta > x_turn_threshold) {
                    // X轴减小（向左倾斜）→ 左转
                    if (x_change < 0) {
                        direction = "左转";
                        detected_emotion = "Turn_left";
                    }
                    // X轴增大（向右倾斜）→ 右转
                    else if (x_change > 0) {
                        direction = "右转";
                        detected_emotion = "Turn_right";
                    }
                }
            }
            
            // 如果检测到方向变化，进行计数（类似眩晕检测）
            if (detected_emotion != nullptr) {
                // 如果方向改变，重置计数器
                if (last_detected_direction != nullptr && strcmp(detected_emotion, last_detected_direction) != 0) {
                    direction_count = 0;
                }
                last_detected_direction = detected_emotion;
                direction_count++;
                
                // 检查是否达到触发阈值且已过冷却时间
                if (direction_count >= direction_count_threshold && 
                    current_time - board->last_direction_time_ >= direction_cooldown) {
                    // 方向检测（前后左右）只能在轮播模式下触发
                    auto display_mode = board->display_->GetDisplayMode();
                    if (display_mode != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                        // 不是轮播模式，跳过方向检测触发
                        direction_count = 0;
                        board->last_direction_time_ = current_time;
                        continue;
                    }
                    
                    ESP_LOGI("LIS2HH12", "🔄 陀螺仪触发: 方向=%s, X轴=%d, Y轴=%d, 表情=%s", 
                             direction, x_raw, y_raw, detected_emotion);
                    
                    // 只有在轮播模式下才记录用户操作并重置定时器
                    board->has_user_interaction_ = true;
                    board->last_interaction_time_ = current_time;
                    if (board->power_save_timer_) {
                        board->power_save_timer_->ResetTimer();
                    }
                    
                    if (Application::GetInstance().IsTmpFactoryTestMode()) {
                        board->display_->UpdateTestItem("sensor", 1);
                    } else {
                        // 轮播模式下：保存轮播状态，触发表情，启动恢复定时器
                        if (!board->gyro_emotion_active_) {
                            board->gyro_emotion_active_ = true;
                            ESP_LOGI("LIS2HH12", "保存轮播状态，准备恢复轮播");
                        }
                        
                        // 触发表情（会切换到固定表情模式）
                        board->display_->SetEmotion(detected_emotion);
                        ESP_LOGI("LIS2HH12", "已触发表情: %s (轮播模式，5秒后恢复轮播)", detected_emotion);
                        
                        // 创建或重启恢复定时器（5秒后恢复轮播）
                        if (board->gyro_emotion_restore_timer_ == nullptr) {
                            esp_timer_create_args_t timer_args = {
                                .callback = [](void* arg) {
                                    MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                    // 根据当前显示模式决定恢复逻辑
                                    auto display_mode = board->display_->GetDisplayMode();
                                    if (display_mode == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                        // 如果已经在轮播模式，说明是方向检测触发的，直接返回（已经恢复）
                                        ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，已在轮播模式，无需恢复");
                                        board->gyro_emotion_active_ = false;
                                    } else if (display_mode == EyeDisplay::DisplayMode::FIXED_EMOTION) {
                                        // 如果是固定表情模式，说明是摇晃检测触发的，恢复之前保存的表情
                                        if (!board->saved_emotion_before_gyro_.empty()) {
                                            ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复表情: %s", 
                                                     board->saved_emotion_before_gyro_.c_str());
                                            board->display_->SetEmotion(board->saved_emotion_before_gyro_.c_str());
                                            board->saved_emotion_before_gyro_.clear();
                                        } else {
                                            ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式（未保存表情）");
                                            board->display_->ToggleVideoCyclingMode();
                                        }
                                        board->gyro_emotion_active_ = false;
                                    } else {
                                        // 其他模式，恢复轮播模式
                                        ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式");
                                        board->display_->ToggleVideoCyclingMode();
                                        board->gyro_emotion_active_ = false;
                                    }
                                    // 注意：不更新 last_direction_time_，因为触发时已经更新过了
                                    // 如果在这里更新，会导致冷却时间从恢复时重新开始，需要再等5秒才能触发
                                },
                                .arg = board,
                                .name = "gyro_emotion_restore"
                            };
                            esp_timer_create(&timer_args, &board->gyro_emotion_restore_timer_);
                        }
                        esp_timer_stop(board->gyro_emotion_restore_timer_);  // 先停止（如果正在运行）
                        esp_timer_start_once(board->gyro_emotion_restore_timer_, 5000000);  // 5秒 = 5000000微秒
                    }
                    
                    // 重置计数和更新触发时间
                    direction_count = 0;
                    board->last_direction_time_ = current_time;
                }
            } else {
                // 没有检测到方向变化，减少计数（类似摇晃检测的衰减）
                if (direction_count > 0) {
                    direction_count -= direction_count_decay;
                    if (direction_count < 0) {
                        direction_count = 0;
                    }
                }
                // 如果长时间没有检测到方向，清空上次方向记录
                if (last_detected_direction != nullptr && direction_count == 0) {
                    last_detected_direction = nullptr;
                }
            }
            
            // 更新last值（用于下次计算变化量）
            if (!first_reading) {
                last_x_raw = x_raw;
                last_y_raw = y_raw;
            }
            
            // 检测是否有明显的总加速度变化（摇晃检测，优先级低于方向检测）
            if (delta_total > threshold) {
                shake_count++;
                
                if (shake_count >= shake_count_threshold) {
                    // 检查是否已经过了冷却时间
                    if (current_time - last_shake_time >= shake_cooldown) {
                        // 摇晃检测只能在轮播模式和固定表情模式下触发，配网模式下不触发
                        auto display_mode = board->display_->GetDisplayMode();
                        if (display_mode == EyeDisplay::DisplayMode::WIFI_CONFIG) {
                            // 配网模式，跳过摇晃检测触发
                            shake_count = 0;
                            last_shake_time = current_time;
                            continue;
                        }
                        
                        // 检查是否在允许的模式下（轮播模式或固定表情模式）
                        if (display_mode != EyeDisplay::DisplayMode::VIDEO_CYCLING && 
                            display_mode != EyeDisplay::DisplayMode::FIXED_EMOTION) {
                            // 不是允许的模式，跳过
                            shake_count = 0;
                            last_shake_time = current_time;
                            continue;
                        }
                        
                        ESP_LOGI("LIS2HH12", "🔄 陀螺仪触发: 方向=摇晃, 总加速度=%.3f, 变化量=%.3f, 表情=vertigo", 
                                 total_accel, delta_total);
                        ESP_LOGI("LIS2HH12", "触发时数据: X=%.3f, Y=%.3f, Z=%.3f", ax, ay, az);
                        last_shake_time = current_time; // 更新上次触发时间
                        shake_count = 0; // 触发后清零

                        // 只有在轮播模式下才记录用户操作并重置定时器
                        if (display_mode == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                            board->has_user_interaction_ = true;
                            board->last_interaction_time_ = current_time;
                            if (board->power_save_timer_) {
                                board->power_save_timer_->ResetTimer();
                            }
                        }

                        if (Application::GetInstance().IsTmpFactoryTestMode()) {
                            board->display_->UpdateTestItem("sensor", 1);
                        } else {
                            // 固定表情模式下：保存当前表情，触发表情，启动恢复定时器
                            if (display_mode == EyeDisplay::DisplayMode::FIXED_EMOTION) {
                                // 如果之前没有保存表情，保存当前表情（可能是默认的neutral）
                                if (!board->gyro_emotion_active_) {
                                    board->saved_emotion_before_gyro_ = "neutral";  // 默认恢复为neutral
                                    board->gyro_emotion_active_ = true;
                                    ESP_LOGI("LIS2HH12", "保存当前表情: %s", board->saved_emotion_before_gyro_.c_str());
                                }
                                
                                // 触发表情
                                board->display_->SetEmotion("vertigo");
                                ESP_LOGI("LIS2HH12", "已触发表情: vertigo (固定表情模式，5秒后恢复)");
                                
                                // 创建或重启恢复定时器（5秒后恢复）
                                // 注意：如果定时器已经存在（由方向检测创建），直接使用，不重新创建
                                if (board->gyro_emotion_restore_timer_ == nullptr) {
                                    esp_timer_create_args_t timer_args = {
                                        .callback = [](void* arg) {
                                            MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                            // 根据当前显示模式决定恢复逻辑（统一恢复逻辑）
                                            auto display_mode = board->display_->GetDisplayMode();
                                            if (display_mode == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                                // 如果已经在轮播模式，说明是方向检测触发的，直接返回（已经恢复）
                                                ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，已在轮播模式，无需恢复");
                                                board->gyro_emotion_active_ = false;
                                            } else if (display_mode == EyeDisplay::DisplayMode::FIXED_EMOTION) {
                                                // 如果是固定表情模式，说明是摇晃检测触发的，恢复之前保存的表情
                                                if (!board->saved_emotion_before_gyro_.empty()) {
                                                    ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复表情: %s", 
                                                             board->saved_emotion_before_gyro_.c_str());
                                                    board->display_->SetEmotion(board->saved_emotion_before_gyro_.c_str());
                                                    board->saved_emotion_before_gyro_.clear();
                                                } else {
                                                    ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式（未保存表情）");
                                                    board->display_->ToggleVideoCyclingMode();
                                                }
                                                board->gyro_emotion_active_ = false;
                                            } else {
                                                // 其他模式，恢复轮播模式
                                                ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式");
                                                board->display_->ToggleVideoCyclingMode();
                                                board->gyro_emotion_active_ = false;
                                            }
                                            // 注意：不更新 last_direction_time_，因为触发时已经更新过了
                                            // 如果在这里更新，会导致方向检测的冷却时间从恢复时重新开始
                                        },
                                        .arg = board,
                                        .name = "gyro_emotion_restore"
                                    };
                                    esp_timer_create(&timer_args, &board->gyro_emotion_restore_timer_);
                                }
                                esp_timer_stop(board->gyro_emotion_restore_timer_);  // 先停止（如果正在运行）
                                esp_timer_start_once(board->gyro_emotion_restore_timer_, 5000000);  // 5秒 = 5000000微秒
                                
                                // 如果Channel打开，发送AI消息
                                if (board->ChannelIsOpen()) {
                                    Application::GetInstance().SendTextToAI("用户正在摇晃你");
                                }
                            } else if (display_mode == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                // 轮播模式下：保存轮播状态，触发表情，启动恢复定时器
                                if (!board->gyro_emotion_active_) {
                                    board->gyro_emotion_active_ = true;
                                    ESP_LOGI("LIS2HH12", "保存轮播状态，准备恢复轮播");
                                }
                                
                                // 触发表情（会切换到固定表情模式）
                                board->display_->SetEmotion("vertigo");
                                ESP_LOGI("LIS2HH12", "已触发表情: vertigo (轮播模式，5秒后恢复轮播)");
                                
                                // 创建或重启恢复定时器（5秒后恢复轮播）
                                // 注意：如果定时器已经存在（由方向检测或固定表情模式下的摇晃检测创建），直接使用，不重新创建
                                if (board->gyro_emotion_restore_timer_ == nullptr) {
                                    esp_timer_create_args_t timer_args = {
                                        .callback = [](void* arg) {
                                            MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                            // 根据当前显示模式决定恢复逻辑（统一恢复逻辑）
                                            auto display_mode = board->display_->GetDisplayMode();
                                            if (display_mode == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                                // 如果已经在轮播模式，说明是方向检测触发的，直接返回（已经恢复）
                                                ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，已在轮播模式，无需恢复");
                                                board->gyro_emotion_active_ = false;
                                            } else if (display_mode == EyeDisplay::DisplayMode::FIXED_EMOTION) {
                                                // 如果是固定表情模式，说明是摇晃检测触发的，恢复之前保存的表情
                                                if (!board->saved_emotion_before_gyro_.empty()) {
                                                    ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复表情: %s", 
                                                             board->saved_emotion_before_gyro_.c_str());
                                                    board->display_->SetEmotion(board->saved_emotion_before_gyro_.c_str());
                                                    board->saved_emotion_before_gyro_.clear();
                                                } else {
                                                    ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式（未保存表情）");
                                                    board->display_->ToggleVideoCyclingMode();
                                                }
                                                board->gyro_emotion_active_ = false;
                                            } else {
                                                // 其他模式，恢复轮播模式
                                                ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式");
                                                board->display_->ToggleVideoCyclingMode();
                                                board->gyro_emotion_active_ = false;
                                            }
                                            // 注意：不更新 last_direction_time_，因为触发时已经更新过了
                                            // 如果在这里更新，会导致冷却时间从恢复时重新开始，需要再等5秒才能触发
                                        },
                                        .arg = board,
                                        .name = "gyro_emotion_restore"
                                    };
                                    esp_timer_create(&timer_args, &board->gyro_emotion_restore_timer_);
                                }
                                esp_timer_stop(board->gyro_emotion_restore_timer_);  // 先停止（如果正在运行）
                                esp_timer_start_once(board->gyro_emotion_restore_timer_, 5000000);  // 5秒 = 5000000微秒
                                
                                // 如果Channel打开，发送AI消息
                                if (board->ChannelIsOpen()) {
                                    Application::GetInstance().SendTextToAI("用户正在摇晃你");
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
            
            last_total_accel = total_accel;
            vTaskDelay(pdMS_TO_TICKS(100)); // 100ms采样间隔
        }
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
        
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel IO creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
            .bits_per_pixel = 16,
        };
        
        esp_lcd_panel_handle_t panel = nullptr;
        ret = esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_reset(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
            return;
        }
        
        ret = esp_lcd_panel_init(panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Invert colors for GC9A01
        ret = esp_lcd_panel_invert_color(panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel color invert failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Mirror display
        ret = esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel mirror failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Turn on display
        ret = esp_lcd_panel_disp_on_off(panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel display on failed: %s", esp_err_to_name(ret));
            return;
        }
        
        display_ = new EyeDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            &qrcode_img,
            {
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),
            });
    }

    int MaxBacklightBrightness() {
        return 20;
    }

    void InitializeChargingGpio() {
        ESP_LOGI(TAG, "开始初始化充电检测IO口: CHARGING_PIN=%d", CHARGING_PIN);
        
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << STANDBY_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        esp_err_t ret1 = gpio_config(&io_conf);
        if (ret1 != ESP_OK) {
            ESP_LOGE(TAG, "❌ STANDBY_PIN初始化失败: GPIO=%d, 错误: %s (0x%x)", 
                     STANDBY_PIN, esp_err_to_name(ret1), ret1);
        } else {
            ESP_LOGI(TAG, "✅ STANDBY_PIN初始化成功: GPIO=%d", STANDBY_PIN);
        }

        gpio_config_t io_conf2 = {
            .pin_bit_mask = (1ULL << CHARGING_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        esp_err_t ret2 = gpio_config(&io_conf2);
        if (ret2 != ESP_OK) {
            ESP_LOGE(TAG, "❌ CHARGING_PIN初始化失败: GPIO=%d (用于充电检测), 错误: %s (0x%x)", 
                     CHARGING_PIN, esp_err_to_name(ret2), ret2);
        } else {
            ESP_LOGI(TAG, "✅ CHARGING_PIN初始化成功: GPIO=%d (用于充电检测)", CHARGING_PIN);
        }
        
        // 打印最终状态
        if (ret1 == ESP_OK && ret2 == ESP_OK) {
            ESP_LOGI(TAG, "✅ 所有充电检测IO口初始化成功");
        } else {
            ESP_LOGW(TAG, "⚠️ 充电检测IO口初始化状态: STANDBY_PIN=%s, CHARGING_PIN=%s", 
                     ret1 == ESP_OK ? "成功" : "失败", ret2 == ESP_OK ? "成功" : "失败");
        }
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "first_level: %d", first_level);

        touch_button_.OnPressDown([this]() {
            return;
          
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
                    // 注意：按钮唤醒时不应该自动进入聆听模式
                    // 只有在检测到唤醒词时，才会通过 WakeWordInvoke() 或 OnWakeWordDetected() 进入聆听模式
                    return;
                }
                display_->SetEmotion("loving");
                if (ChannelIsOpen()) {
                    Application::GetInstance().SendTextToAI("用户正在抚摸你");
                } else {
                    ESP_LOGI("touch", "Channel is not open");
                    Application::GetInstance().ToggleChatState();
                }
            } else {
                ESP_LOGI("touch", "Touch detected but in cooldown period");
            }
        });

        boot_button_.OnClick([this]() {
            if (Application::GetInstance().IsTmpFactoryTestMode()) {
                // 通过按键测试
                display_->UpdateTestItem("key", 1);
                return;
            }

            if (CheckAndHandleEnterSleepMode()) {
                // 交给休眠逻辑托管
                ESP_LOGI(TAG, "长按唤醒");
                // 注意：按钮唤醒时不应该自动进入聆听模式
                // 只有在检测到唤醒词时，才会通过 WakeWordInvoke() 或 OnWakeWordDetected() 进入聆听模式
                return;
            }
            
            // 移除轮播模式下的 ToggleCyclingLock() 调用，让 OnRepeatDone 处理所有点击逻辑
            // 包括轮播模式下的锁定/解锁操作
        });
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
            if (need_power_off_) {
                need_power_off_ = false;
                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    board->display_->SetEmotion("neutral");

                    if (board->IsCharging()) {
                        // 充电中，进入睡眠模式（会自动切换到轮播模式）
                        // 进入睡眠状态前，先切换到轮播模式
                        if(board->display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING){
                            ESP_LOGI("MovecallMojiESP32S3", "长按按键（充电中），进入轮播离线模式");
                            Application::GetInstance().QuitTalking();
                            board->display_->ToggleVideoCyclingMode();
                        }
                        Application::GetInstance().EnterSleepMode();
                    } else {
                        // 没有充电，关机
                        board->PowerOff();
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        boot_button_.OnPressRepeaDone([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnRepeatDone: %d", count);
            if(count == 3){
                InnerResetWifiConfiguration();
            } else if(count == 2){
                // 配网模式下，双击按钮不显示电量和图标
                if (display_->GetDisplayMode() != EyeDisplay::DisplayMode::WIFI_CONFIG) {
                    display_->ShowWifiSignalAndBattery();
                } else {
                    ESP_LOGI(TAG, "配网模式下，双击按钮无效");
                }
            } else if(count == 1){
                // 按一下：如果在轮播模式，锁定/解锁轮播；否则切换聊天状态
                if (display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                    // 轮播模式：记录用户操作并重置定时器
                    has_user_interaction_ = true;
                    last_interaction_time_ = xTaskGetTickCount();
                    if (power_save_timer_) {
                        power_save_timer_->ResetTimer();
                    }
                    // 切换锁定状态（锁定当前视频/继续轮播）
                    display_->ToggleCyclingLock();
                } else {
                    // 非轮播模式：切换聊天状态
                    auto& app = Application::GetInstance();
                    app.ToggleChatState();
                }
            }
            // 移除按四下进入轮播模式的功能，改为在睡眠状态时自动进入
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker")); 
        thing_manager.AddThing(iot::CreateThing("Screen"));   
    }

    // 初始化设备状态变化监听
    void InitializeDeviceStateCallback() {
        ESP_LOGI(TAG, "开始注册设备状态变化监听，display_=%p", display_);
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState prev, DeviceState curr) {
                ESP_LOGI(TAG, "[回调] 设备状态变化: %d -> %d, kDeviceStateSleeping=%d", prev, curr, kDeviceStateSleeping);
                
                // 当设备进入睡眠状态时，停止轮播模式定时器
                bool is_sleeping = (curr == kDeviceStateSleeping || curr == 9);
                if (is_sleeping) {
                    ESP_LOGI(TAG, "[回调] 设备进入睡眠状态，停止轮播模式睡眠计时定时器");
                    StopVideoCyclingSleepTimer();
                }
                
                // 当设备状态变为空闲状态时，切换到轮播模式
                if (curr == kDeviceStateIdle) {
                    if (display_ == nullptr) {
                        return;
                    }
                    // 如果是从睡眠状态进入空闲状态，不需要再次切换（已经在睡眠状态时切换过了）
                    bool is_from_sleeping = (prev == kDeviceStateSleeping || prev == 9);
                    if (is_from_sleeping) {
                        ESP_LOGI(TAG, "[回调] 从睡眠状态进入空闲状态，跳过轮播切换（已在睡眠状态切换过）");
                        // 注意：从睡眠状态唤醒后，不应该自动进入聆听模式
                        // 只有在检测到唤醒词时，才会通过 WakeWordInvoke() 或 OnWakeWordDetected() 进入聆听模式
                        return;
                    }
                    ESP_LOGI(TAG, "[回调] 设备进入空闲状态，切换到轮播模式");
                    auto current_mode = display_->GetDisplayMode();
                    if (current_mode != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                        ESP_LOGI(TAG, "[回调] 切换到轮播模式");
                        Application::GetInstance().QuitTalking();
                        display_->ToggleVideoCyclingMode();
                    }
                    // 第一个定时器触发的睡眠，音频已在OnEnterSleepMode中播放，这里不需要再播放
                    // 使用 Application::Schedule 来延迟确保轮播模式，避免 SetEmotion 退出轮播
                    Application::GetInstance().Schedule([this]() {
                        if (display_ != nullptr && 
                            display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                            ESP_LOGI(TAG, "[回调] 空闲状态后确保轮播模式");
                            display_->ToggleVideoCyclingMode();
                        }
                    }, "ensure_video_cycling_after_idle");
                    return;
                }
                
                // 当设备状态变为睡眠状态时，根据标志决定是否切换到轮播模式，并显示相应表情
                // 注意：is_sleeping 已在上面声明，这里直接使用
                if (is_sleeping) {
                    if (display_ == nullptr) {
                        return;
                    }
                    
                    // 如果是第二个定时器触发的睡眠，已经设置了"Charging"表情，跳过这里的设置和音频播放
                    if (is_second_timer_sleep_) {
                        ESP_LOGI(TAG, "[回调] 第二个定时器触发的睡眠，已设置充电动画（吃电池表情），跳过表情设置和音频播放");
                        auto backlight = GetBacklight();
                        if (backlight) {
                            backlight->SetBrightness(20, false);  // 充电时降低亮度
                        }
                        is_second_timer_sleep_ = false;  // 清除标志
                        // 跳过后续的轮播模式切换逻辑，因为已经在OnShutdownRequest中处理了
                        return;
                    }
                    
                    // 板级逻辑：检查电量和充电状态,决定显示哪个表情
                    int level = 0;
                    bool charging = false;
                    bool discharging = false;
                    bool has_battery = GetBatteryLevel(level, charging, discharging);
                    auto backlight = GetBacklight();
                    
                    // 只有在特定条件下才显示充电表情
                    bool should_show_charging_emotion = false;
                    if (has_battery) {
                        // 只有在第二个定时器触发时（即通过OnShutdownRequest）才显示充电表情
                        // 这里我们只处理正常的睡眠状态显示
                        if (level < 25 && !charging) {
                            // 低电量(未充电),显示吃电池表情
                            ESP_LOGI(TAG, "[回调] 低电量进入睡眠模式(电量: %d%%),显示吃电池表情", level);
                            display_->SetEmotion("Charging");
                            // 低电量时关闭背光
                            if (backlight) {
                                backlight->SetBrightness(0);
                            }
                            should_show_charging_emotion = true;
                        } else {
                            // 正常情况显示睡觉表情
                            ESP_LOGI(TAG, "[回调] 进入睡眠模式(电量: %d%%, 充电: %d),显示睡觉表情", level, charging);
                            display_->SetEmotion("sleepy");
                            // 根据充电状态设置背光亮度
                            if (backlight) {
                                if (charging) {
                                    // 充电时降低亮度
                                    backlight->SetBrightness(20, false);
                                } else {
                                    // 非充电时恢复正常亮度
                                    backlight->RestoreBrightness();
                                }
                            }
                        }
                    } else {
                        // 无法获取电量信息,默认显示睡觉表情
                        display_->SetEmotion("sleepy");
                        // 无法获取电量信息时根据充电状态设置背光
                        if (backlight) {
                            if (charging) {
                                backlight->SetBrightness(20, false);
                            } else {
                                backlight->RestoreBrightness();
                            }
                        }
                    }
                    
                    // 注意：音频播放由 Application::EnterSleepMode 统一处理，这里不重复播放
                    
                    // 如果是第一个定时器触发的睡眠（sleep_with_video_cycling_=true），确保是轮播模式
                    if (sleep_with_video_cycling_) {
                        ESP_LOGI(TAG, "[回调] 第一个定时器触发的睡眠，确保轮播模式");
                        auto current_mode = display_->GetDisplayMode();
                        if (current_mode != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                            ESP_LOGI(TAG, "[回调] 切换到轮播模式");
                            display_->ToggleVideoCyclingMode();
                        }
                        // 注意：EnterSleepMode 中的 SetEmotion 会异步执行，但我们在 Application::Schedule 之后
                        // 通过 Application::Schedule 来延迟确保轮播模式，避免 SetEmotion 退出轮播
                        Application::GetInstance().Schedule([this]() {
                            if (display_ != nullptr && 
                                display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                ESP_LOGI(TAG, "[回调] EnterSleepMode后确保轮播模式");
                                display_->ToggleVideoCyclingMode();
                            }
                            // 第一个定时器触发时，如果是轮播模式，恢复屏幕亮度（保持显示）
                            if (display_ != nullptr && 
                                display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                auto backlight = GetBacklight();
                                if (backlight != nullptr) {
                                    ESP_LOGI(TAG, "[回调] 轮播模式下进入睡眠，恢复屏幕亮度");
                                    backlight->RestoreBrightness();
                                }
                            }
                            sleep_with_video_cycling_ = false;  // 清除标志
                        }, "ensure_video_cycling_after_sleep");
                    } else {
                        // 其他情况（如启动时直接进入睡眠），检查是否应该切换到轮播模式
                        // 如果是第二个定时器触发的睡眠（显示充电表情），不应该切换到轮播模式
                        if (is_second_timer_sleep_) {
                            ESP_LOGI(TAG, "[回调] 第二个定时器触发的睡眠（显示充电表情），不切换到轮播模式");
                            is_second_timer_sleep_ = false;  // 清除标志
                            return;  // 直接返回，不执行后续逻辑
                        }
                        
                        // 其他情况（如启动时直接进入睡眠），也切换到轮播模式
                        ESP_LOGI(TAG, "[回调] 其他情况进入睡眠状态，切换到轮播模式");
                        auto current_mode = display_->GetDisplayMode();
                        if (current_mode != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                            ESP_LOGI(TAG, "[回调] 切换到轮播模式");
                            Application::GetInstance().QuitTalking();
                            display_->ToggleVideoCyclingMode();
                        }
                        // 轮播模式下恢复背光亮度
                        if (display_ != nullptr && 
                            display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                            if (backlight != nullptr) {
                                ESP_LOGI(TAG, "[回调] 轮播模式下进入睡眠，恢复屏幕亮度");
                                backlight->RestoreBrightness();
                            }
                        }
                        // 使用 Application::Schedule 来延迟确保轮播模式，避免 SetEmotion 退出轮播
                        Application::GetInstance().Schedule([this]() {
                            if (display_ != nullptr && 
                                display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                ESP_LOGI(TAG, "[回调] EnterSleepMode后确保轮播模式");
                                display_->ToggleVideoCyclingMode();
                            }
                            // 确保轮播模式下背光是开启的
                            if (display_ != nullptr && 
                                display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                auto backlight = GetBacklight();
                                if (backlight != nullptr) {
                                    ESP_LOGI(TAG, "[回调] 确保轮播模式下背光开启");
                                    backlight->RestoreBrightness();
                                }
                            }
                        }, "ensure_video_cycling_after_sleep_other");
                    }
                }
            }
        );
        ESP_LOGI(TAG, "设备状态变化监听已注册完成");
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

    void InnerResetWifiConfiguration() {
        // 强制拉低背光 io
        // gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);
        // vTaskDelay(pdMS_TO_TICKS(10));
        ResetWifiConfiguration();
    }

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
    }
    
    // 修复唤醒后不能进入聆听模式的问题
    void HandleWakeFromChargingSleep() {
        auto& app = Application::GetInstance();
        // 如果是从充电睡眠状态唤醒，则主动进入聆听模式
        if (app.GetDeviceState() == kDeviceStateIdle) {
            ESP_LOGI(TAG, "从充电睡眠状态唤醒，进入聆听模式");
            app.ToggleChatState();
        }
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
            // ESP_LOGE(TAG, "Failed to create LIS2HH12 I2C bus: %s", esp_err_to_name(ret));
            return;
        }
        
        ESP_LOGI(TAG, "LIS2HH12 I2C bus initialized successfully");
        // 给传感器一些启动时间（LIS2HH12 上电后需要约 10-50ms 才能响应）
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 尝试检测LIS2HH12的I2C地址（0x1D或0x1E）
    bool DetectLis2hh12Address(uint8_t& detected_addr) {
        const uint8_t possible_addrs[] = {0x1D, 0x1E};
        const int MAX_RETRIES = 3;  // 每个地址重试3次
        const uint32_t I2C_SPEED_HZ = 100000;  // 降低到100kHz，更稳定
        
        for (int i = 0; i < 2; i++) {
            uint8_t test_addr = possible_addrs[i];
            ESP_LOGI(TAG, "尝试检测LIS2HH12地址: 0x%02X", test_addr);
            
            // 创建临时设备句柄进行测试
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = test_addr,
                .scl_speed_hz = I2C_SPEED_HZ,  // 降低速度提高稳定性
            };
            
            i2c_master_dev_handle_t test_dev = nullptr;
            esp_err_t ret = i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &test_dev);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "无法添加设备到地址 0x%02X: %s", test_addr, esp_err_to_name(ret));
                continue;
            }
            
            // 重试机制：每个地址尝试多次
            for (int retry = 0; retry < MAX_RETRIES; retry++) {
                if (retry > 0) {
                    ESP_LOGI(TAG, "地址 0x%02X 重试第 %d 次", test_addr, retry + 1);
                    vTaskDelay(pdMS_TO_TICKS(50));  // 重试前等待
                }
                
                // 尝试读取WHO_AM_I寄存器（0x0F）
                uint8_t reg = 0x0F;
                uint8_t data = 0;
                ret = i2c_master_transmit_receive(test_dev, &reg, 1, &data, 1, pdMS_TO_TICKS(500));
                
                if (ret == ESP_OK && data == 0x41) {
                    // 找到正确的地址
                    detected_addr = test_addr;
                    ESP_LOGI(TAG, "LIS2HH12检测成功！地址: 0x%02X, WHO_AM_I: 0x%02X (重试 %d 次)", 
                             test_addr, data, retry + 1);
                    // 删除临时设备
                    i2c_master_bus_rm_device(test_dev);
                    return true;
                } else if (ret == ESP_OK) {
                    ESP_LOGW(TAG, "地址 0x%02X 有响应但WHO_AM_I不正确: 0x%02X (期望0x41)", test_addr, data);
                } else {
                    ESP_LOGD(TAG, "地址 0x%02X 无响应 (重试 %d/%d): %s", 
                             test_addr, retry + 1, MAX_RETRIES, esp_err_to_name(ret));
                }
            }
            
            // 删除临时设备
            i2c_master_bus_rm_device(test_dev);
            vTaskDelay(pdMS_TO_TICKS(20)); // 尝试下一个地址前延迟
        }
        
        ESP_LOGE(TAG, "未找到LIS2HH12设备（尝试了0x1D和0x1E，每个地址重试%d次）", MAX_RETRIES);
        return false;
    }

    void InitializeLis2hh12() {
        // 首先自动检测I2C地址
        if (!DetectLis2hh12Address(lis2hh12_i2c_addr_)) {
            ESP_LOGE(TAG, "LIS2HH12地址检测失败，无法初始化");
            lis2hh12_dev_ = nullptr;
            return;
        }
        
        // 使用检测到的地址创建设备句柄
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = lis2hh12_i2c_addr_,
            .scl_speed_hz = 100000,  // 使用100kHz，与检测时一致
        };
        esp_err_t ret = i2c_master_bus_add_device(lis2hh12_i2c_bus_, &dev_cfg, &lis2hh12_dev_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add LIS2HH12 device at address 0x%02X: %s", 
                     lis2hh12_i2c_addr_, esp_err_to_name(ret));
            lis2hh12_dev_ = nullptr;
            return;
        }
        
        // 再次验证WHO_AM_I寄存器
        uint8_t who_am_i = this->lis2hh12_read_reg(0x0F);
        ESP_LOGI(TAG, "LIS2HH12 WHO_AM_I: 0x%02X (地址: 0x%02X)", who_am_i, lis2hh12_i2c_addr_);
        
        if (who_am_i != 0x41) {
            ESP_LOGE(TAG, "LIS2HH12 WHO_AM_I验证失败! Expected 0x41, got 0x%02X", who_am_i);
            i2c_master_bus_rm_device(lis2hh12_dev_);
            lis2hh12_dev_ = nullptr;
            return;
        }
        
        ESP_LOGI(TAG, "LIS2HH12 detected successfully at address 0x%02X", lis2hh12_i2c_addr_);
        
        // 0x20: CTRL1, 0x57 = 100Hz, all axes enable, normal mode
        this->lis2hh12_write_reg(0x20, 0x57);
        // 0x23: CTRL4, 0x00 = continuous update, LSB at lower address
        this->lis2hh12_write_reg(0x23, 0x00);
        
        ESP_LOGI(TAG, "LIS2HH12 initialized successfully");
    }

    // LIS2HH12 I2C读写成员函数
    uint8_t lis2hh12_read_reg(uint8_t reg) {
        if (lis2hh12_dev_ == nullptr) {
            // 设备未初始化，直接返回0，避免错误日志
            return 0;
        }
        uint8_t data = 0;
        esp_err_t ret = i2c_master_transmit_receive(lis2hh12_dev_, &reg, 1, &data, 1, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            // ESP_LOGE(TAG, "LIS2HH12 read reg 0x%02X failed: %s", reg, esp_err_to_name(ret));
            return 0;
        }
        return data;
    }
    
    void lis2hh12_write_reg(uint8_t reg, uint8_t value) {
        if (lis2hh12_dev_ == nullptr) {
            // 设备未初始化，直接返回，避免错误日志
            return;
        }
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
                
                // 显示充电环（隐藏背景颜色，只显示进度条）
                if (display_ != nullptr) {
                    display_->ShowBatteryLevel(false);  // false=隐藏背景颜色
                }
                
                // 重要：充电时停止轮播定时器
                StopVideoCyclingSleepTimer();
                
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");

                // 隐藏充电环
                if (display_ != nullptr) {
                    display_->HiddenBatteryLevel();
                }

                if (this->is_charging_sleep_) {
                    // 充电停止时，不关机，而是进入轮播离线模式
                    ESP_LOGI(TAG, "充电停止，进入轮播离线模式");
                    // 进入睡眠状态前，先切换到轮播模式
                    if(display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING){
                        Application::GetInstance().QuitTalking();
                        display_->ToggleVideoCyclingMode();
                    }
                    Application::GetInstance().EnterSleepMode();
                    is_charging_sleep_ = false;
                }
            }

            // 通知 mqtt 
            auto& mqtt_client = MqttClient::getInstance();
            mqtt_client.ReportTimer();

        });
    }

public:
    MovecallMojiESP32S3() : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO) { 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);

        // 设置I2C master日志级别为ERROR，忽略I2C事务失败的日志
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        
        InitializeChargingGpio();

        InitializeGpio(POWER_GPIO, true);

        InitializeFlash();
        InitializeI2c();
        InitializeGpio(AUDIO_CODEC_PA_PIN, true);
        // InitializeGpio(DISPLAY_BACKLIGHT_PIN, false);
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeLis2hh12I2c(); // 新增LIS2HH12专用I2C
        InitializeLis2hh12();    // 初始化LIS2HH12
        
        // 检查I2C设备是否正常
        if (lis2hh12_dev_ == nullptr) {
            ESP_LOGE(TAG, "LIS2HH12 device not initialized, skipping sensor task");
        } else {
            ESP_LOGI(TAG, "LIS2HH12 device initialized successfully");
            // 启动检测任务
            xTaskCreatePinnedToCore(MovecallMojiESP32S3::lis2hh12_task, "lis2hh12_task", 1024 * 3, this, 1, NULL, 0);
        }
        InitializeButtons();
        InitializeIot();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeDeviceStateCallback();  // 注册设备状态变化监听
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
        ESP_LOGI(TAG, "PowerOff");
        gpio_set_level(POWER_GPIO, 0);
    }

    virtual void WakeWordDetected() override {
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
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool IsCharging() override {
        return power_manager_->IsCharging();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
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
    
    // 检查LIS2HH12设备是否已初始化
    bool is_lis2hh12_initialized() const { return lis2hh12_dev_ != nullptr; }
};

DECLARE_BOARD(MovecallMojiESP32S3);