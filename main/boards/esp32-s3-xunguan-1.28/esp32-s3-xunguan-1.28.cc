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
    
    // 陀螺仪表情恢复定时器（固定表情模式下使用）
    esp_timer_handle_t gyro_emotion_restore_timer_ = nullptr;
    std::string saved_emotion_before_gyro_;  // 陀螺仪触发前保存的表情
    bool gyro_emotion_active_ = false;  // 是否正在显示陀螺仪触发的表情
    TickType_t last_direction_time_ = 0;  // 上次方向触发时间（用于重置冷却时间）

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
        float last_total_accel = 0.0f;
        const float threshold = 0.45f; // g-force (摇晃检测阈值)
        int shake_count = 0;
        const int shake_count_threshold = 3; // 连续3次检测到变化才触发摇晃
        const int shake_count_decay = 1;     // 每次没检测到就-1
        TickType_t last_shake_time = 0;      // 上次摇晃触发时间
        const TickType_t shake_cooldown = pdMS_TO_TICKS(5000); // 5秒冷却时间
        
        // 方向检测阈值（使用变化量检测，类似眩晕检测）
        // 使用raw值的变化量，避免静止状态误触发
        const int16_t x_turn_threshold = 800;   // X轴变化量超过此值判断为转向（与前后阈值一致）
        const int16_t y_forward_threshold = 800;  // Y轴变化量超过此值且为正向变化判断为前进
        const int16_t y_backward_threshold = 800; // Y轴变化量超过此值且为负向变化判断为后退
        
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
                    ESP_LOGI("LIS2HH12", "🔄 陀螺仪触发: 方向=%s, X轴=%d, Y轴=%d, 表情=%s", 
                             direction, x_raw, y_raw, detected_emotion);
                    
                    if (Application::GetInstance().IsTmpFactoryTestMode()) {
                        board->display_->UpdateTestItem("sensor", 1);
                    } else {
                        // 固定表情模式下：保存当前表情，触发表情，启动恢复定时器
                        if (board->display_->GetDisplayMode() == EyeDisplay::DisplayMode::FIXED_EMOTION) {
                            // 如果之前没有保存表情，保存当前表情（可能是默认的neutral）
                            if (!board->gyro_emotion_active_) {
                                // 获取当前表情（通过视频组索引反推，或使用默认值）
                                board->saved_emotion_before_gyro_ = "neutral";  // 默认恢复为neutral
                                board->gyro_emotion_active_ = true;
                                ESP_LOGI("LIS2HH12", "保存当前表情: %s", board->saved_emotion_before_gyro_.c_str());
                            }
                            
                            // 触发表情
                            board->display_->SetEmotion(detected_emotion);
                            ESP_LOGI("LIS2HH12", "已触发表情: %s (固定表情模式，5秒后恢复)", detected_emotion);
                            
                            // 创建或重启恢复定时器（5秒后恢复）
                            if (board->gyro_emotion_restore_timer_ == nullptr) {
                                esp_timer_create_args_t timer_args = {
                                    .callback = [](void* arg) {
                                        MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                        ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复表情: %s", 
                                                 board->saved_emotion_before_gyro_.c_str());
                                        board->display_->SetEmotion(board->saved_emotion_before_gyro_.c_str());
                                        board->gyro_emotion_active_ = false;
                                        board->saved_emotion_before_gyro_.clear();
                                    },
                                    .arg = board,
                                    .name = "gyro_emotion_restore"
                                };
                                esp_timer_create(&timer_args, &board->gyro_emotion_restore_timer_);
                            }
                            esp_timer_stop(board->gyro_emotion_restore_timer_);  // 先停止（如果正在运行）
                            esp_timer_start_once(board->gyro_emotion_restore_timer_, 5000000);  // 5秒 = 5000000微秒
                        } else if (board->display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
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
                                        ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式");
                                        // 恢复轮播模式
                                        board->display_->ToggleVideoCyclingMode();
                                        board->gyro_emotion_active_ = false;
                                        // 重置方向检测冷却时间，允许立即检测新方向
                                        board->last_direction_time_ = 0;
                                    },
                                    .arg = board,
                                    .name = "gyro_emotion_restore"
                                };
                                esp_timer_create(&timer_args, &board->gyro_emotion_restore_timer_);
                            }
                            esp_timer_stop(board->gyro_emotion_restore_timer_);  // 先停止（如果正在运行）
                            esp_timer_start_once(board->gyro_emotion_restore_timer_, 5000000);  // 5秒 = 5000000微秒
                        } else {
                            // 其他模式：直接触发表情，不恢复
                            board->display_->SetEmotion(detected_emotion);
                            ESP_LOGI("LIS2HH12", "已触发表情: %s", detected_emotion);
                        }
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
                        ESP_LOGI("LIS2HH12", "🔄 陀螺仪触发: 方向=摇晃, 总加速度=%.3f, 变化量=%.3f, 表情=vertigo", 
                                 total_accel, delta_total);
                        ESP_LOGI("LIS2HH12", "触发时数据: X=%.3f, Y=%.3f, Z=%.3f", ax, ay, az);
                        last_shake_time = current_time; // 更新上次触发时间
                        shake_count = 0; // 触发后清零

                        if (Application::GetInstance().IsTmpFactoryTestMode()) {
                            board->display_->UpdateTestItem("sensor", 1);
                        } else {
                            // 固定表情模式下：保存当前表情，触发表情，启动恢复定时器
                            if (board->display_->GetDisplayMode() == EyeDisplay::DisplayMode::FIXED_EMOTION) {
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
                                if (board->gyro_emotion_restore_timer_ == nullptr) {
                                    esp_timer_create_args_t timer_args = {
                                        .callback = [](void* arg) {
                                            MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                            ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复表情: %s", 
                                                     board->saved_emotion_before_gyro_.c_str());
                                            board->display_->SetEmotion(board->saved_emotion_before_gyro_.c_str());
                                            board->gyro_emotion_active_ = false;
                                            board->saved_emotion_before_gyro_.clear();
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
                            } else if (board->display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                                // 轮播模式下：保存轮播状态，触发表情，启动恢复定时器
                                if (!board->gyro_emotion_active_) {
                                    board->gyro_emotion_active_ = true;
                                    ESP_LOGI("LIS2HH12", "保存轮播状态，准备恢复轮播");
                                }
                                
                                // 触发表情（会切换到固定表情模式）
                                board->display_->SetEmotion("vertigo");
                                ESP_LOGI("LIS2HH12", "已触发表情: vertigo (轮播模式，5秒后恢复轮播)");
                                
                                // 创建或重启恢复定时器（5秒后恢复轮播）
                                if (board->gyro_emotion_restore_timer_ == nullptr) {
                                    esp_timer_create_args_t timer_args = {
                                        .callback = [](void* arg) {
                                            MovecallMojiESP32S3* board = static_cast<MovecallMojiESP32S3*>(arg);
                                            ESP_LOGI("LIS2HH12", "⏰ 陀螺仪表情5秒到期，恢复轮播模式");
                                            // 恢复轮播模式
                                            board->display_->ToggleVideoCyclingMode();
                                            board->gyro_emotion_active_ = false;
                                            // 重置方向检测冷却时间，允许立即检测新方向
                                            board->last_direction_time_ = 0;
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
                            } else {
                                // 其他模式：直接触发表情，不恢复
                                if (board->ChannelIsOpen()) {
                                    board->display_->SetEmotion("vertigo");
                                    Application::GetInstance().SendTextToAI("用户正在摇晃你");
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

            if (display_->GetDisplayMode() == EyeDisplay::DisplayMode::VIDEO_CYCLING) {
                // 轮播模式：切换锁定状态（锁定当前视频/继续轮播）
                display_->ToggleCyclingLock();
                return;
            }

            if (Application::GetInstance().IsTmpFactoryTestMode()) {
                // 通过按键测试
                display_->UpdateTestItem("key", 1);
                return;
            }


            if (CheckAndHandleEnterSleepMode()) {
                // 交给休眠逻辑托管
                ESP_LOGI(TAG, "长按唤醒");
                return;
            }
            auto& app = Application::GetInstance();
            // if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
            //     InnerResetWifiConfiguration();
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

        boot_button_.OnPressRepeaDone([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnRepeatDone: %d", count);
            if(count == 3){
                InnerResetWifiConfiguration();
            } else if(count == 2){
                display_->ShowWifiSignalAndBattery();
            } else if(count == 4){
                // 判断屏幕当前模式
                if(display_->GetDisplayMode() != EyeDisplay::DisplayMode::VIDEO_CYCLING){
                    // 退出聊天状态
                    ESP_LOGI(TAG, "进入轮播模式");
                    Application::GetInstance().QuitTalking();
                } else {
                    ESP_LOGI(TAG, "进入固定表情模式");
                    // 如果当前在配网模式，则重新回到配网模式
                    if (wifi_config_mode_) {
                        display_->ToggleVideoCyclingMode();
                        display_->EnterWifiConfig();
                        return;
                    }
                }

                display_->ToggleVideoCyclingMode();
            }
        });
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
                
                // 显示充电环
                if (display_ != nullptr) {
                    display_->ShowBatteryLevel();
                }
                
            } else {
                // 充电停止时的处理逻辑
                ESP_LOGI(TAG, "检测到停止充电");

                // 隐藏充电环
                if (display_ != nullptr) {
                    display_->HiddenBatteryLevel();
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
        // InitializeLis2hh12I2c(); // 新增LIS2HH12专用I2C
        // InitializeLis2hh12();    // 初始化LIS2HH12
        
        // 检查I2C设备是否正常
        if (lis2hh12_dev_ == nullptr) {
            ESP_LOGE(TAG, "LIS2HH12 device not initialized, skipping sensor task");
        } else {
            ESP_LOGI(TAG, "LIS2HH12 device initialized successfully");
        }
        InitializeButtons();
        InitializeIot();
        // xTaskCreatePinnedToCore(MovecallMojiESP32S3::lis2hh12_task, "lis2hh12_task", 1024 * 3, this, 1, NULL, 0); // 启动检测任务
        InitializePowerManager();
        InitializePowerSaveTimer();
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