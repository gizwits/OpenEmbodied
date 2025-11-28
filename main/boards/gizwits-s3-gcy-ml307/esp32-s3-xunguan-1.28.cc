#include "dual_network_board.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "audio/codecs/vb6824_audio_codec.h"

#include "iot/thing_manager.h"
#include "power_manager.h"
#include "gc_data_point_manager.h"

#include "assets/lang_config.h"
#include "font_awesome_symbols.h"
#include "wifi_connection_manager.h"


#include "led/single_led.h"
#include "display/lcd_display.h"
#include "display/display.h"

#include <wifi_station.h>
#include "power_save_timer.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include <time.h>
#include <sys/time.h>

#include <math.h>

#define TAG "MovecallMojiESP32S3"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class MovecallMojiESP32S3 : public DualNetworkBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button reset_button_;
    Button break_button_;
    SpiLcdDisplay* display_;

    bool need_power_off_ = false;
    VbAduioCodec audio_codec;

    i2c_master_bus_handle_t i2c_bus_;
    int64_t power_on_time_ = 0;  // 记录上电时间
    PowerManager* power_manager_;
    PowerSaveTimer* power_save_timer_;
    bool is_charging_sleep_ = false;
    esp_timer_handle_t alarm_check_timer_ = nullptr;  // 闹钟检查定时器

    // std::vector<TestItem> test_items = {
    //     {"lcd", "LCD测试", 1},
    //     {"key", "按键测试", 0},
    //     {"wifi", "WiFi连接测试", 0},
    //     {"sensor", "陀螺仪测试", 0},
    //     {"battery", "电池检测", 0},
    //     {"mic", "麦克风检测", 0},
    // };

    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {
        "你好小古","哥哥在吗","老师在吗","老公在吗","老婆在吗","宝宝在吗"
    };
    std::vector<std::string> network_config_words_ = {"开始配网"};



    void InitializeDataPointManager() {
        // 设置 GCDataPointManager 的回调函数
        GCDataPointManager::GetInstance().SetCallbacks(
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
                this->GetBacklight()->SetBrightness(value, true);
            },
            [this](const std::string& url) {
                display_->DownloadBackgroundImage(url);
            },
            [this](const std::string& url) {
                // 创建结构体来传递 board 和 url
                display_->DownloadBackgroundVideo(url);
            },
            // 新增数据点的回调函数
            []() -> bool { return true; }, // get_switch_callback - 默认开启
            [](bool value) { /* TODO: 实现开关功能 */ }, // set_switch_callback
            []() -> bool { return true; }, // get_wakeup_word_callback - 默认开启
            [](bool value) { /* TODO: 实现唤醒词开关功能 */ }, // set_wakeup_word_callback
            []() -> int { return 0; }, // get_alert_tone_language_callback - 0=中文
            [](int value) { /* TODO: 实现提示音语言切换功能 */ }, // set_alert_tone_language_callback
            []() -> int { return 0; }, // get_speed_callback - 默认语速0
            [](int value) { /* TODO: 实现语速设置功能 */ }, // set_speed_callback
            [](int index) -> uint32_t { return 0; }, // get_timer_callback - 默认返回0
            [](int index, uint32_t value) { /* TODO: 实现闹钟设置功能 */ }, // set_timer_callback
            [](int index, const std::string& text) { /* TODO: 实现闹钟文字提示设置功能 */ } // set_tts_callback
        );
    }


    void InitializePowerSaveTimer() {
        // 20 分钟进休眠
        // 30 分钟 关机
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 5, 60 * 10);
        // power_save_timer_ = new PowerSaveTimer(-1, 20 * 1, 60 * 2);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGE(TAG, "Enabling sleep mode");
            if(IsCharging()) {
                // 充电中
                is_charging_sleep_ = true;
                Application::GetInstance().Schedule([this]() {
                    Application::GetInstance().QuitTalking();
                    this->GetBacklight()->SetBrightness(0, false);
                    // Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);

                    // 在这个场景里要切换成睡觉表情 
                    // display_->SetEmotion("sleepy");
                }, "EnterSleepMode_QuitTalking");

            } else {
                // 关闭 wifi，进入待机模式
                // Application::GetInstance().EnterSleepMode();
                // 直接关机
                PowerOff();
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

    // 打印 Unicode 文本（支持 UTF-8 和 UTF-16），返回解码后的 UTF-8 文本
    std::string PrintUnicodeText(const std::string& text, const char* label = "文本") {
        if (text.empty()) {
            return "";
        }
        
        // 如果是 UTF-16，固定使用 BE 解码（如果有 BOM 则按 BOM 指示）
        if (text.length() >= 2 && (text.length() % 2 == 0)) {
            bool is_le = false;
            size_t start = 0;
            
            // 检查 BOM: FE FF (UTF-16 BE) 或 FF FE (UTF-16 LE)
            if (text.length() >= 2) {
                uint8_t b0 = static_cast<uint8_t>(text[0]);
                uint8_t b1 = static_cast<uint8_t>(text[1]);
                
                if ((b0 == 0xFE && b1 == 0xFF) || (b0 == 0xFF && b1 == 0xFE)) {
                    is_le = (b0 == 0xFF && b1 == 0xFE);
                    start = 2;  // 跳过 BOM
                }
            }
            
            std::string utf8_text;
            
            for (size_t j = start; j + 1 < text.length(); j += 2) {
                uint16_t code_point;
                if (is_le) {
                    // UTF-16 LE
                    code_point = static_cast<uint8_t>(text[j]) | 
                                 (static_cast<uint8_t>(text[j + 1]) << 8);
                } else {
                    // UTF-16 BE（默认）
                    code_point = (static_cast<uint8_t>(text[j]) << 8) | 
                                 static_cast<uint8_t>(text[j + 1]);
                }
                
                if (code_point == 0) {
                    break;
                }
                
                // 转换为 UTF-8
                if (code_point < 0x80) {
                    utf8_text += static_cast<char>(code_point);
                } else if (code_point < 0x800) {
                    utf8_text += static_cast<char>(0xC0 | (code_point >> 6));
                    utf8_text += static_cast<char>(0x80 | (code_point & 0x3F));
                } else {
                    utf8_text += static_cast<char>(0xE0 | (code_point >> 12));
                    utf8_text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
                    utf8_text += static_cast<char>(0x80 | (code_point & 0x3F));
                }
            }
            
            if (!utf8_text.empty()) {
                return utf8_text;  // 返回解码后的 UTF-8 文本
            }
        }
        
        // 如果不是 UTF-16 或解码失败，返回原始文本（可能是 UTF-8）
        return text;
    }

    // 检查闹钟并执行提醒
    void CheckAlarms() {
        time_t now;
        time(&now);
        
        // 获取当前时间
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        ESP_LOGI(TAG, "═══════════════════════════════════════");
        ESP_LOGI(TAG, "⏰ 开始检查闹钟");
        ESP_LOGI(TAG, "当前时间: %04d-%02d-%02d %02d:%02d:%02d (时间戳: %ld)",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, now);
        
        // 检查 timer1-timer10
        for (int i = 1; i <= 10; i++) {
            std::string timer_name = "timer" + std::to_string(i);
            uint32_t timer_value = 0;
            bool from_cache = false;
            
            // ESP_LOGI(TAG, "─────────────────────────────────────");
            // ESP_LOGI(TAG, "检查 timer%d (%s)", i, timer_name.c_str());
            
            // 从缓存或存储中获取闹钟时间戳
            int cached_int_value = 0;
            if (GCDataPointManager::GetInstance().GetCachedDataPoint(timer_name, cached_int_value)) {
                from_cache = true;
                // 如果缓存值是 -1，说明原始值超过了 int32_t 范围，需要从字符串读取
                if (cached_int_value == -1) {
                    Settings settings("datapoint", false);
                    std::string str_val = settings.GetString(timer_name, "");
                    if (!str_val.empty()) {
                        try {
                            timer_value = std::stoul(str_val);
                            // ESP_LOGI(TAG, "  ✓ 从缓存(-1标记)和NVS字符串读取 timer_value: %s -> %u (0x%08X)", 
                            //          str_val.c_str(), timer_value, timer_value);
                        } catch (const std::exception& e) {
                            ESP_LOGW(TAG, "  ✗ 字符串转换失败: %s", e.what());
                        }
                    }
                } else {
                    timer_value = static_cast<uint32_t>(cached_int_value);
                    // ESP_LOGI(TAG, "  ✓ 从缓存读取 timer_value: %u (0x%08X)", timer_value, timer_value);
                }
            } else {
                // 如果缓存中没有，尝试从存储读取
                Settings settings("datapoint", false);
                int32_t int_val = settings.GetInt(timer_name, -1);
                // ESP_LOGI(TAG, "  ✓ 从NVS读取 timer_value (int): %d (0x%08X)", int_val, int_val);
                
                // 如果 int 读取失败或值可能被截断，尝试从字符串读取
                if (int_val == -1 || int_val == 0) {
                    std::string str_val = settings.GetString(timer_name, "");
                    if (!str_val.empty()) {
                        try {
                            timer_value = std::stoul(str_val);
                            // ESP_LOGI(TAG, "  ✓ 从NVS字符串读取 timer_value: %s -> %u (0x%08X)", 
                            //          str_val.c_str(), timer_value, timer_value);
                        } catch (const std::exception& e) {
                            ESP_LOGW(TAG, "  ✗ 字符串转换失败: %s", e.what());
                        }
                    } else if (int_val == 0) {
                        timer_value = 0;
                    }
                } else {
                    // 检查 int 值是否可能被截断（如果原始值超过 INT32_MAX，应该存储为字符串）
                    timer_value = static_cast<uint32_t>(int_val);
                    // ESP_LOGI(TAG, "  ✓ 使用NVS int值 timer_value: %u (0x%08X)", timer_value, timer_value);
                }
            }
            
            if (timer_value == 0) {
                // ESP_LOGI(TAG, "  → timer%d 未设置，跳过", i);
                continue; // 未设置的闹钟跳过
            }
            
            // timer_value 是完整的时间戳（包括日期和时间）
            // ESP_LOGI(TAG, "  原始 timer_value: %u (0x%08X)", timer_value, timer_value);
            time_t alarm_t = static_cast<time_t>(timer_value);
            // ESP_LOGI(TAG, "  转换后 alarm_t: %ld (0x%016lX)", alarm_t, alarm_t);
            
            struct tm alarm_timeinfo;
            localtime_r(&alarm_t, &alarm_timeinfo);
            // ESP_LOGI(TAG, "  闹钟完整时间戳: %ld", alarm_t);
            // ESP_LOGI(TAG, "  闹钟日期时间: %04d-%02d-%02d %02d:%02d:%02d",
            //          alarm_timeinfo.tm_year + 1900, alarm_timeinfo.tm_mon + 1, alarm_timeinfo.tm_mday,
            //          alarm_timeinfo.tm_hour, alarm_timeinfo.tm_min, alarm_timeinfo.tm_sec);
            
            // 直接比较完整时间戳（允许30秒误差）
            const int TIME_TOLERANCE_SECONDS = 30;
            int64_t time_diff = static_cast<int64_t>(now) - static_cast<int64_t>(alarm_t);
            int64_t abs_time_diff = (time_diff < 0) ? -time_diff : time_diff;
            
            // ESP_LOGI(TAG, "  当前时间戳: %ld", now);
            // ESP_LOGI(TAG, "  闹钟时间戳: %ld", alarm_t);
            // ESP_LOGI(TAG, "  时间差: %lld 秒 (阈值: %d 秒)", time_diff, TIME_TOLERANCE_SECONDS);
            
            // 获取并打印对应的 TTS 文本（无论是否触发都打印）
            std::string tts_name = "tts" + std::to_string(i);
            Settings settings("datapoint", false);
            std::string tts_text_raw = settings.GetString(tts_name, "");
            std::string tts_text = "";  // 解码后的文本
            
            if (!tts_text_raw.empty()) {
                // 使用封装的方法打印 Unicode 文本，并获取解码后的文本
                tts_text = PrintUnicodeText(tts_text_raw, "TTS文本");
            } else {
                // ESP_LOGI(TAG, "     TTS文本: (未设置)");
            }
            
            // 如果闹钟时间已经过期（超过阈值），跳过
            if (time_diff > TIME_TOLERANCE_SECONDS) {
                // ESP_LOGI(TAG, "  → timer%d 已过期 (时间差 %lld 秒 > 阈值 %d 秒)", 
                //          i, time_diff, TIME_TOLERANCE_SECONDS);
                continue;
            }
            
            // 如果闹钟时间在未来（超过阈值），还未到时间
            if (time_diff < -TIME_TOLERANCE_SECONDS) {
                // ESP_LOGI(TAG, "  → timer%d 未到时间 (时间差 %lld 秒 < -阈值 %d 秒)", 
                //          i, time_diff, -TIME_TOLERANCE_SECONDS);
                continue;
            }
            
            // 在误差范围内，触发闹钟
            if (abs_time_diff <= TIME_TOLERANCE_SECONDS) {
                // ESP_LOGI(TAG, "  ⚡ 闹钟 timer%d 触发！", i);
                // ESP_LOGI(TAG, "     当前时间: %04d-%02d-%02d %02d:%02d:%02d (时间戳: %ld)",
                //          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                //          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, now);
                // ESP_LOGI(TAG, "     闹钟时间: %04d-%02d-%02d %02d:%02d:%02d (时间戳: %ld)",
                //          alarm_timeinfo.tm_year + 1900, alarm_timeinfo.tm_mon + 1, alarm_timeinfo.tm_mday,
                //          alarm_timeinfo.tm_hour, alarm_timeinfo.tm_min, alarm_timeinfo.tm_sec, alarm_t);
                // ESP_LOGI(TAG, "     时间差: %lld 秒", time_diff);
                
                // TTS 文本已经在上面打印过了，这里只需要处理播放逻辑
                if (!tts_text.empty()) {
                    static bool need_wait_connect = false;
                    if (!Application::GetInstance().IsAudioChannelOpened()) {
                        need_wait_connect = true;
                        Application::GetInstance().ToggleChatState();
                        // 没连接就发起链接
                    }
                    Application::GetInstance().Schedule([this, tts_text]() {
                        // 临时禁用音频上传，避免闹钟播放时上传麦克风音频
                        ESP_LOGI(TAG, "     临时禁用音频上传，准备播放闹钟提醒");
                        Application::GetInstance().SetAudioUploadEnabled(false);
                        
                        if (need_wait_connect) {
                            // 等待连接成功
                            vTaskDelay(pdMS_TO_TICKS(2000));
                        }

                        Application::GetInstance().SetDeviceState(kDeviceStateSpeaking);
                        ESP_LOGI(TAG, "     播放闹钟提醒: %s", tts_text.c_str());
                        Application::GetInstance().GenerateTTSFromText(tts_text);
                        
                        // 发送完 TTS 请求后，延迟一段时间再恢复音频上传
                        // 给 TTS 音频一些时间开始播放，避免立即恢复上传导致干扰
                        vTaskDelay(pdMS_TO_TICKS(5000));
                        ESP_LOGI(TAG, "     恢复音频上传");
                        Application::GetInstance().SetAudioUploadEnabled(true);
                    }, "PlayTTS_Alarm");
                } else {
                    ESP_LOGI(TAG, "     闹钟 timer%d 触发，但未设置提醒文本", i);
                    // 可以播放默认提示音
                    Application::GetInstance().Schedule([this, tts_text]() {
                        Application::GetInstance().ResetDecoder();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        Application::GetInstance().PlaySound(Lang::Sounds::P3_SUCCESS);
                    });
                    // Application::GetInstance().PlaySound(Lang::Sounds::P3_ALARM);
                }
            } else {
                // ESP_LOGI(TAG, "  → timer%d 未到时间 (时间差 %d 秒 > 阈值 %d 秒)", 
                //          i, time_diff, TIME_TOLERANCE_SECONDS);
            }
        }
        ESP_LOGI(TAG, "═══════════════════════════════════════");
    }

    // 闹钟检查定时器回调
    static void AlarmCheckTimerCallback(void* arg) {
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        self->CheckAlarms();
    }

    void InitializeAlarmCheckTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = AlarmCheckTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "alarm_check_timer",
            .skip_unhandled_events = true,
        };
        
        esp_err_t ret = esp_timer_create(&timer_args, &alarm_check_timer_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "创建闹钟检查定时器失败: %s", esp_err_to_name(ret));
            return;
        }
        
        // 每30秒检查一次
        ret = esp_timer_start_periodic(alarm_check_timer_, 30 * 1000000ULL); // 30秒 = 30 * 1000000 微秒
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "启动闹钟检查定时器失败: %s", esp_err_to_name(ret));
            esp_timer_delete(alarm_check_timer_);
            alarm_check_timer_ = nullptr;
            return;
        }
        
        ESP_LOGI(TAG, "闹钟检查定时器已启动，每30秒检查一次");
    }

    virtual void ResetPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->ResetTimer();
        }
    };

    virtual void WakeUpPowerSaveTimer() {
        is_charging_sleep_ = false;
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(true);
            power_save_timer_->WakeUp();
        }
    };
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

    // ST7789W3初始化 (240x296)
    void InitializeSt7789Display() {
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
            .rgb_ele_order = DISPLAY_RGB_ORDER,
            .bits_per_pixel = 16,
        };
        
        esp_lcd_panel_handle_t panel = nullptr;
        ret = esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel);
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

        // Set column/row gap to align visible window and avoid bottom artifacts
        ret = esp_lcd_panel_set_gap(panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Panel set gap failed: %s", esp_err_to_name(ret));
            return;
        }
        
        // Invert colors for ST7789W3
        ret = esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
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
        
        // display_ = new EyeDisplayHorizontalEmo(panel_io, panel,
        //     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
        //     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
        //     &qrcode_img,
        //     {
        //         .text_font = &font_puhui_20_4,
        //         .icon_font = &font_awesome_20_4,
        //         .emoji_font = font_emoji_64_init(),
        //     });
        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH,
            DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
            DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,{
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),
            });
    }

    int MaxBacklightBrightness() {
        return 100;
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

    // 初始化耳机检测GPIO
    void InitializeHeadphoneDetection() {
        // 配置HPR-SIGN为输入（检测耳机插入）
        gpio_config_t hpr_conf = {
            .pin_bit_mask = (1ULL << HPR_SIGN_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&hpr_conf));

        // 配置MCU MUTE为输出（控制静音）
        gpio_config_t mute_conf = {
            .pin_bit_mask = (1ULL << MCU_MUTE_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&mute_conf));

        // 初始化时读取一次状态并设置MCU MUTE
        UpdateMuteSignal();

        ESP_LOGI(TAG, "Headphone detection GPIO initialized");
    }

    // 更新MCU MUTE信号
    void UpdateMuteSignal() {
        int hpr_level = !gpio_get_level(HPR_SIGN_PIN);
        // HPR-SIGN为高时，有耳机插入，输出MCU MUTE为高
        // HPR-SIGN为低时，无耳机插入，输出MCU MUTE为低
        gpio_set_level(MCU_MUTE_PIN, hpr_level);
        ESP_LOGI(TAG, "HPR-SIGN: %d, MCU MUTE: %d", hpr_level, hpr_level);
    }

    // 耳机检测监控任务
    static void HeadphoneDetectionTask(void* arg) {
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        int last_hpr_level = -1;

        for (;;) {
            int current_hpr_level = gpio_get_level(HPR_SIGN_PIN);
            
            // 如果状态发生变化，更新MCU MUTE
            if (current_hpr_level != last_hpr_level) {
                self->UpdateMuteSignal();
                last_hpr_level = current_hpr_level;
            }

            // 每100ms检查一次
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "first_level: %d", first_level);


        boot_button_.OnClick([this]() {

            if (Application::GetInstance().IsTmpFactoryTestMode()) {
                // 通过按键测试
                // display_->UpdateTestItem("key", 1);
                return;
            }


            if (CheckAndHandleEnterSleepMode()) {
                // 交给休眠逻辑托管
                return;
            }
            

            GetBacklight()->RestoreBrightness();

            auto& app = Application::GetInstance();
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
                this->GetBacklight()->SetBrightness(0, false);
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            first_level = 1;
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                need_power_off_ = false;
                is_charging_sleep_ = true;
                ESP_LOGI(TAG, "设置休眠标志");

                // 使用静态函数来避免lambda捕获问题
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<MovecallMojiESP32S3*>(arg);
                    board->display_->SetEmotion("neutral");
                    Application::GetInstance().QuitTalking();


                    if (board->IsCharging()) {
                        // 充电中，只关闭背光
                        board->GetBacklight()->SetBrightness(0, false);
                        // is_charging_sleep_ 已经在创建Task之前设置了
                    } else {
                        // 没有充电，关机
                        board->PowerOff();
                    }
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });

        reset_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "reset_button_.OnLongPress");
            InnerResetWifiConfiguration();
        });

        reset_button_.OnPressRepeaDone([this](uint16_t count) {
            ESP_LOGI(TAG, "reset_button_.OnPressRepeaDone, count: %d", count);
            if(count == 5){
                SwitchNetworkType();
                return;
            }
        });

        break_button_.OnClick([this]() {
            ESP_LOGI(TAG, "break_button_.OnClick");
            // 休眠模式只有开关可以启动
            if (is_charging_sleep_) {
                return;
            }
            Application::GetInstance().ToggleChatState();
        });
        
        // Volume up button - short press to increase volume
        volume_up_button_.OnPressDown([this]() {
            if (is_charging_sleep_) {
                return;
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume up: %d", volume);
        });
        
        // Volume down button - short press to decrease volume
        volume_down_button_.OnPressDown([this]() {
            if (is_charging_sleep_) {
                return;
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Volume down: %d", volume);
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
        if (GetNetworkType() == NetworkType::WIFI) {
            auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
            wifi_board.ResetWifiConfiguration();
        }
    }

    bool ChannelIsOpen() {
        auto& app = Application::GetInstance();
        return app.GetDeviceState() != kDeviceStateIdle;
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
            if (is_charging) {
                ESP_LOGI(TAG, "检测到开始充电");
            } else {
                ESP_LOGI(TAG, "检测到停止充电");
                if (this->is_charging_sleep_) {
                    ESP_LOGI(TAG, "充电停止，关机");
                    PowerOff();
                }
            }

            // 通知 mqtt 
            // auto& mqtt_client = MqttClient::getInstance();
            // mqtt_client.ReportTimer();

        });
    }

    
    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }
public:
    MovecallMojiESP32S3() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1, UART_NUM_2),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        reset_button_(RESET_BUTTON_GPIO),
        break_button_(BREAK_BUTTON_GPIO),
        audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO) { 
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动，上电时间戳: %lld ms", power_on_time_);

        // 设置I2C master日志级别为ERROR，忽略I2C事务失败的日志
        esp_log_level_set("i2c.master", ESP_LOG_ERROR);
        InitializeHeadphoneDetection();
        InitializeChargingGpio();
        InitializeGpio(POWER_GPIO, true);
        // 根据网络类型设置ML307_EN：Wi-Fi模式时禁用4G模块，4G模式时启用
        if (GetNetworkType() == NetworkType::WIFI) {
            InitializeGpio(ML307_EN, false);  // 禁用4G模块
            ESP_LOGI(TAG, "Wi-Fi模式，禁用4G模块 (ML307_EN = LOW)");
        } else {
            InitializeGpio(ML307_EN, true);   // 启用4G模块
            ESP_LOGI(TAG, "4G模式，启用4G模块 (ML307_EN = HIGH)");
        }

        InitializeSpi();
        InitializeSt7789Display();
        
        InitializeButtons();
        InitializeIot();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeDataPointManager();
        InitializeAlarmCheckTimer();
        // ESP_LOGI(TAG, "ReadADC2_CH1_Oneshot");
        // ReadADC2_CH1_Oneshot();
        if (power_manager_) {
            power_manager_->CheckBatteryStatusImmediately();
            ESP_LOGI(TAG, "启动时立即检测电量: %d", power_manager_->GetBatteryLevel());
        }

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            auto& app = Application::GetInstance();
            
            // 如果是静默启动状态，忽略唤醒词
            if (app.IsSilentStartup()) {
                ESP_LOGI(TAG, "静默启动状态，忽略唤醒词: %s", command.c_str());
                return;
            }
            
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", app.GetDeviceState());
                // if(app.GetDeviceState() != kDeviceStateListening){
                // }
                app.WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                InnerResetWifiConfiguration();
            }
        });
        xTaskCreate(
            RestoreBacklightTask,      // 任务函数
            "restore_backlight",       // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );

        // 启动耳机检测监控任务
        xTaskCreate(
            HeadphoneDetectionTask,    // 任务函数
            "headphone_detect",        // 名字
            4096,                      // 栈大小
            this,                      // 参数传递 this 指针
            5,                         // 优先级
            NULL                       // 任务句柄
        );

        if (Application::GetInstance().IsTmpFactoryTestMode()) {
            // display_->EnterTestMode();
            // display_->SetTestItems(test_items);
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
                // if (ret == ESP_OK) {
                //     display_->UpdateTestItemStatus("wifi", 1);
                // } else {
                //     // 设置失败
                //     display_->UpdateTestItemStatus("wifi", 2);
                // }
            }, "factory_test_mode");

            Application::GetInstance().Schedule([this]() {
                // ADC 电池检测
                vTaskDelay(pdMS_TO_TICKS(1000));
                int level = 0;
                bool charging = false;
                bool discharging = false;
                GetBatteryLevel(level, charging, discharging);
                // 合理范围：1..100 认为有效（0 可能意味着未接电池/异常）
                // if (level >= 1 && level <= 100) {
                //     display_->UpdateTestItemStatus("battery", 1);
                // } else {
                //     display_->UpdateTestItemStatus("battery", 2);
                // }
            }, "adc_test");
        }
    }

    virtual bool GetNeedPlayWakeWordSound() override {
        return false;
    }
    virtual void PowerOff() override {
        gpio_set_level(POWER_GPIO, 0);
    }

    virtual void WakeWordDetected() override {
        ESP_LOGI(TAG, "WakeWordDetected");
        // display_->UpdateTestItemStatus("mic", 1);

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
        vTaskDelay(pdMS_TO_TICKS(300));
        auto* self = static_cast<MovecallMojiESP32S3*>(arg);
        int level;
        bool charging, discharging;
        self->GetBatteryLevel(level, charging, discharging);
        // XunguanDisplay* xunguan_display = static_cast<XunguanDisplay*>(self->GetDisplay());
        self->GetBacklight()->RestoreBrightness();

        vTaskDelete(NULL); // 任务结束时删除自己
    }

    virtual bool NeedToogleIdle() override {
        return true;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual int GetPeriod() override { 
        return 1; 
    }
    
    virtual int GetMaxFrameNum() override { 
        return 20;
    }

    virtual bool IsCharging() override {
        // int chrg = gpio_get_level(CHARGING_PIN);
        // int standby = gpio_get_level(STANDBY_PIN);
        // // return false;
        // return chrg == 0 || standby == 0;
        return power_manager_->IsCharging();
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = IsCharging();
        discharging = !charging;
        // level = 1;
        level = power_manager_->GetBatteryLevel();
        // ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);
        return true;
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }


    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return GCDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return GCDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, uint32_t& value) const override {
        return GCDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, uint32_t value) override {
        return GCDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        GCDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, uint32_t value) override {
        GCDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

    void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) override {
        GCDataPointManager::GetInstance().ProcessBinaryDataPointValue(name, data, data_len);
    }
};

DECLARE_BOARD(MovecallMojiESP32S3);