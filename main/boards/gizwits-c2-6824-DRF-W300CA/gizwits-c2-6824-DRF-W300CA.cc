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
#include "lws_data_point_manager.h"
#include <esp_timer.h>

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include "power_manager.h"
#include "rgb_led.h"
#include "motor_control.h"
#include "esp_adc/adc_oneshot.h"
#include <iot_button.h>
#include "device_state_event.h"

#define TAG "CustomBoard"


#define RESET_WIFI_CONFIGURATION_COUNT 3
#define SLEEP_TIME_SEC 60 * 3

// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;  // BOOT按键(GPIO8)
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    
    // RGB LED 和电机控制实例
    RgbLed rgb_led_;
    MotorControl motor_control_;
    bool motor_on_ = false;
    uint8_t current_led_mode_ = 0;  // 当前LED模式 0-7
    
    // ADC按钮(使用ESP-IDF库)
    AdcButton* adc_button_k50_;
    AdcButton* adc_button_k51_;
    
    // ADC检测相关(仅用于电池检测)
    adc_oneshot_unit_handle_t adc1_handle_ = nullptr;
    // 防交叉触发节流
    int64_t last_k50_click_ms_ = 0;
    int64_t last_k51_click_ms_ = 0;
    int64_t last_k50_longpress_ms_ = 0;  // K50 长按时间，用于延长保护窗口
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人", "小蜜小蜜"};
    std::vector<std::string> network_config_words_ = {"开始配网"};
    
    // RGB灯光状态管理
    bool rgb_light_on_ = false;
    TaskHandle_t rgb_task_handle_ = nullptr;
    bool rainbow_enabled_ = false; // 渐变任务使能标志（任务常驻，按此开关运行）
    
    // K51按键颜色循环状态
    uint8_t k51_color_mode_ = 7; // 0=全彩渐变, 1=白, 2=红, 3=绿, 4=蓝, 5=黄, 6=青, 7=紫

	// 板载指示灯（GPIO_NUM_6）状态与定时器
	esp_timer_handle_t builtin_led_timer_ = nullptr;
	bool builtin_led_state_ = false; // true 表示输出高电平，false 表示输出低电平（该灯低电平点亮）
	
	// 蓝灯状态管理（用于AI状态指示）
	bool blue_led_on_ = false; // 蓝灯是否开启
	
	// K51 延迟打断定时器（用于防止 K50 误触发 K51）
	esp_timer_handle_t k51_abort_timer_ = nullptr;
	bool k51_pending_abort_ = false;  // 是否有待执行的打断操作
	
	// 静默启动标志（充电状态下不自动启动，需要长按BOOT按键）
	bool silent_startup_from_board_ = false;

	static void BuiltinLedTimerCallback(void* arg) {
		CustomBoard* self = static_cast<CustomBoard*>(arg);
		// 静默启动：强制关闭状态灯，避免任何闪烁
		if (self->silent_startup_from_board_) {
			self->builtin_led_state_ = false;
			gpio_set_level(BUILTIN_LED_GPIO, 1); // active-low: 1=OFF
			return;
		}
		// 配网模式下 0.5s 闪烁；其他状态常亮（低电平点亮）
		if (Board::GetInstance().IsWifiConfigMode()) {
			self->builtin_led_state_ = !self->builtin_led_state_;
			gpio_set_level(BUILTIN_LED_GPIO, self->builtin_led_state_ ? 1 : 0);
		} else {
			self->builtin_led_state_ = false;
			gpio_set_level(BUILTIN_LED_GPIO, 0);
		}
	}

    void ApplyLedMode_(uint8_t mode) {
        // 限制模式范围
        if (mode > 7) mode = 7;
        
        // 停止之前的灯光效果
        if (rgb_light_on_) {
            StopRgbLightEffect();
        }
        
        // 更新当前模式
        current_led_mode_ = mode;
        
        // 根据模式应用效果（减少日志打印，避免影响音频）
        switch (mode) {
            case 0:
                StartRgbLightEffect();
                break;
            case 1:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(255, 255, 255);
                break;
            case 2:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(255, 0, 0);
                break;
            case 3:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(0, 255, 0);
                break;
            case 4:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(0, 0, 255);
                break;
            case 5:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(255, 255, 0);
                break;
            case 6:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(0, 255, 255);
                break;
            case 7:
                rgb_light_on_ = true;
                rgb_led_.SetBrightness(MapAppliedBrightness_(GetBrightness_()));
                rgb_led_.SetColor(255, 0, 255);
                break;
        }
        k51_color_mode_ = current_led_mode_; // 让下次按键从当前模式的下一个开始
        // 更新负载补偿
        UpdateBatteryLoadComp();
    }
    
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

	// 根据电机与RGB灯状态，通知电池管理当前系统状态
	void UpdateBatteryLoadComp() {
		uint8_t brightness = MapAppliedBrightness_(GetBrightness_()); // 使用应用亮度(0-100)
		uint8_t motor_speed = motor_on_ ? GetLightSpeed_() : 0; // 电机速度
		bool led_enabled = brightness > 0;
		// 减少日志打印，避免影响音频（只在调试时启用）
		// ESP_LOGI(TAG, "电机数据点: light_speed=%d, motor_on=%s", (int)motor_speed, motor_on_ ? "开" : "关");
		// 更新系统状态到PowerManager
		PowerManager::GetInstance().UpdateSystemStatus(
			motor_on_,      // 电机运行状态
			motor_speed,    // 电机速度
			led_enabled,    // LED使能状态
			brightness,     // LED亮度
			current_led_mode_  // LED模式
		);
		
	}
	
	// 控制板载状态灯（GPIO_NUM_6，AI状态指示）
	void SetBlueLed(bool on) {
		if (blue_led_on_ == on) {
			// 状态未变化，但如果是要关闭，仍然强制关闭（防止状态不一致）
			if (!on) {
				ESP_LOGI(TAG, "状态灯已关闭，强制确保关闭状态");
				gpio_set_level(BUILTIN_LED_GPIO, 1);  // 高电平熄灭
				// 停止定时器，避免定时器干扰
				if (builtin_led_timer_) {
					esp_timer_stop(builtin_led_timer_);
				}
			}
			return;
		}
		blue_led_on_ = on;
		
		// 停止定时器，避免定时器干扰我们的控制
		if (builtin_led_timer_) {
			esp_timer_stop(builtin_led_timer_);
		}
		
		if (on) {
			// 开启状态灯：低电平点亮（该灯低电平点亮）
			gpio_set_level(BUILTIN_LED_GPIO, 0);
			ESP_LOGI(TAG, "状态灯开启（AI激活）");
		} else {
			// 关闭状态灯：高电平熄灭
			gpio_set_level(BUILTIN_LED_GPIO, 1);
			ESP_LOGI(TAG, "状态灯关闭（AI休眠）");
		}
	}
	
	// 初始化设备状态事件监听
	void InitializeDeviceStateEvent() {
		DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
			[this](DeviceState prev, DeviceState curr) {
				ESP_LOGI(TAG, "设备状态变化: %d -> %d", prev, curr);
				
				// 静默启动时，不响应状态变化，保持状态灯关闭
				if (silent_startup_from_board_) {
					SetBlueLed(false);
					return;
				}
				
				// 配网模式：恢复定时器，让定时器控制灯闪烁
				if (curr == kDeviceStateWifiConfiguring) {
					if (builtin_led_timer_) {
						esp_timer_start_periodic(builtin_led_timer_, 500000); // 500ms
					}
					return;
				}
				
				// 真正的休眠状态：sleeping -> 关闭状态灯
				// idle 状态时设备还在运行，保持状态灯开启（表示设备在线）
				if (curr == kDeviceStateSleeping) {
					SetBlueLed(false);
					return;
				}
				
				// AI激活状态：listening 或 speaking -> 开启状态灯
				if (curr == kDeviceStateListening || curr == kDeviceStateSpeaking) {
					SetBlueLed(true);
					return;
				}
				
				// 其他状态保持当前状态灯状态不变
			}
		);
	}

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();
        
        // 进入休眠时，关闭状态灯
        SetBlueLed(false);

		// 统一休眠策略：无论充电或未充电，都不进入深度休眠
		// 目的：保持 RGB 与电机运行，避免被关断
		ESP_LOGI(TAG, "🔋 统一休眠策略：不进入深度休眠（充电/未充电一致处理）");
    }
    
    // 设备关机方法
    // 关机行为与 xingbao 项目保持一致：拉低保持脚；若在充电，则重启以进入静默/充电逻辑
    virtual void PowerOff() override {
        ESP_LOGI(TAG, "PowerOff called");
        
        // 等待电量不足播报完成（如果正在播报）
        ESP_LOGI(TAG, "等待电量不足播报完成...");
        vTaskDelay(pdMS_TO_TICKS(3000));  // 等待3秒让电量不足播报完成
        
        // 关闭所有功能
        StopRgbLightEffect();
        motor_control_.Stop();
        motor_on_ = false;
        device_powered_on_ = false;
        
        // 检查充电状态，如果在充电则重启（silent_next已在长按时保存）
        bool is_charging = PowerManager::GetInstance().IsCharging();
        if (is_charging) {
            // 充电场景统一按 xingbao 行为：重启进入静默充电
            ESP_LOGI(TAG, "USB充电模式，重启设备以检测NVS标志");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return;
        }
        
        // 电池模式下，关机前播报休眠提示音
        {
            auto codec = GetAudioCodec();
            if (codec) {
                codec->EnableOutput(true);
            }
            Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        
        // 拉低电源保持引脚，关闭电池供电
        gpio_set_level(POWER_HOLD_GPIO, 0);
        ESP_LOGI(TAG, "🔋 电源保持引脚已拉低，设备关机 (GPIO%d)", POWER_HOLD_GPIO);
        
        // 延时3秒
        vTaskDelay(pdMS_TO_TICKS(3000));

        // 进入深度睡眠
        run_sleep_mode(false);
    }
    
    // 返回是否需要在充电时静默启动
    virtual bool NeedSilentStartup() override {
        return silent_startup_from_board_;
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
            
            // 静默启动时，K50按键无效
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，K50按键无效");
                return;
            }
            
            if (!device_powered_on_) {
                // 设备关机状态,不响应
                ESP_LOGI(TAG, "设备关机状态,K50按键无效");
                return;
            }

            // 防止刚触发K51后短时间内误触发K50(缩短时间窗口，提高响应速度)
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_k51_click_ms_ < 100) {  // 从250ms缩短到100ms
                ESP_LOGI(TAG, "K50按下但与K51间隔过短,忽略");
                return;
            }
            
            // 如果 K51 有待执行的打断操作，取消它（防止切换灯光时误打断AI）
            if (k51_pending_abort_) {
                if (k51_abort_timer_) {
                    esp_timer_stop(k51_abort_timer_);
                    esp_timer_delete(k51_abort_timer_);
                    k51_abort_timer_ = nullptr;
                }
                k51_pending_abort_ = false;
                ESP_LOGI(TAG, "K50按下，取消K51的待执行打断操作");
            }
            
            last_k50_click_ms_ = now_ms;
            
            // 根据数据点判断当前状态
            uint8_t dp_brightness = GetBrightness_();
            uint8_t dp_light_speed = GetLightSpeed_();
            
            if (dp_brightness == 0) {
                // 亮度为0，开启灯光、电机和模式
                uint8_t current_mode = GetLightMode_();
                k51_color_mode_ = current_mode;
                
                // 检查电机速度数据点，如果为0则设置为100
                if (dp_light_speed == 0) {
                    LWSDataPointManager::GetInstance().SetCachedDataPoint("light_speed", 100);
                    dp_light_speed = 100;
                }
                
                // 启动电机
                motor_control_.SetSpeed(dp_light_speed);
                motor_control_.Start();

                // 设置亮度为80
                LWSDataPointManager::GetInstance().SetCachedDataPoint("brightness", 80);
                
                // 应用灯光模式
                ApplyLedMode_(current_mode);
                // 同步数据点（不触发回调）
                LWSDataPointManager::GetInstance().SetCachedDataPoint("light_mode", current_mode);
            } else {
                // 亮度不为0，切换到下一个模式
                uint8_t current_mode = GetLightMode_();
                uint8_t next_mode = (current_mode + 1) % 8;
                
                // 检查电机速度数据点，如果为0则设置为100
                if (dp_light_speed == 0) {
                    LWSDataPointManager::GetInstance().SetCachedDataPoint("light_speed", 100);
                    dp_light_speed = 100;
                }
                
                // 启动电机（使用现有速度）
                motor_control_.SetSpeed(dp_light_speed);
                motor_control_.Start();
                
                ApplyLedMode_(next_mode);
                // 同步数据点（不触发回调）
                LWSDataPointManager::GetInstance().SetCachedDataPoint("light_mode", next_mode);
            }
        });
        
        // 设置K50按钮长按回调 - 关闭灯光和电机
        adc_button_k50_->OnLongPress([this]() {
            ESP_LOGI(TAG, " ===== K50按键长按 =====");
            ESP_LOGI(TAG, " 按键类型: K50按键 (ADC检测)");
            ESP_LOGI(TAG, " 检测范围: 0-200 (0V-0.16V)");
            
            // 静默启动时，K50长按无效
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，K50长按无效");
                return;
            }
            
            // 设备关机状态下不响应
            if (!device_powered_on_) {
                ESP_LOGI(TAG, "设备关机状态,K50长按无效");
                return;
            }
            
            // 记录长按时间，用于延长保护窗口
            int64_t now_ms = esp_timer_get_time() / 1000;
            last_k50_longpress_ms_ = now_ms;
            
            // 如果 K51 有待执行的打断操作，取消它（防止长按关闭灯光时误打断AI）
            if (k51_pending_abort_) {
                if (k51_abort_timer_) {
                    esp_timer_stop(k51_abort_timer_);
                    esp_timer_delete(k51_abort_timer_);
                    k51_abort_timer_ = nullptr;
                }
                k51_pending_abort_ = false;
                ESP_LOGI(TAG, "K50长按，取消K51的待执行打断操作");
            }
            
            // 设备开机状态,强制关闭灯光和电机
            ESP_LOGI(TAG, "强制关闭RGB灯光效果");
            
            // 强制关闭电机
            motor_control_.Stop();
            motor_on_ = false;
            ESP_LOGI(TAG, "🔧 电机已关闭");
            
            // 关闭时将数据点设置为0（使用SetCachedDataPoint避免触发回调）
            LWSDataPointManager::GetInstance().SetCachedDataPoint("brightness", 0);
            LWSDataPointManager::GetInstance().SetCachedDataPoint("light_speed", 0);
            LWSDataPointManager::GetInstance().SetCachedDataPoint("light_mode", 0);
            ESP_LOGI(TAG, "💡 已关闭灯光，数据点亮度设置为0");
            ESP_LOGI(TAG, "🔧 已关闭电机，数据点速度设置为0");
            ESP_LOGI(TAG, "🎨 已关闭灯光模式，数据点模式设置为0");
            
            // 最后关闭RGB灯光效果
            StopRgbLightEffect();
            
            // 重置颜色状态,下次按键从模式0开始
            k51_color_mode_ = 7; // 设为7,这样第一次按键时(7+1)%8=0

			// 更新负载补偿状态
			UpdateBatteryLoadComp();
        });
        
        // 设置K51按钮短按回调 - 打断AI（延迟执行，防止K50误触发）
        adc_button_k51_->OnPressDown([this]() {
            ESP_LOGI(TAG, " ===== K51按键短按 =====");
            ESP_LOGI(TAG, " 按键类型: K51按键 (ADC检测)");
            ESP_LOGI(TAG, " 检测范围: 1500-2500 (1.21V-2.01V)");
            
            // 静默启动时，K51按键无效
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，K51按键无效");
                return;
            }
            
            // 设备关机状态下不响应
            if (!device_powered_on_) {
                ESP_LOGI(TAG, "设备关机状态,K51按键无效");
                return;
            }

            int64_t now_ms2 = esp_timer_get_time() / 1000;
            
            // 防止刚触发K50后短时间内误触发K51（双向保护）
            if (now_ms2 - last_k50_click_ms_ < 100) {
                ESP_LOGI(TAG, "K51按下但与K50间隔过短,忽略");
                return;
            }
            
            // 防止K50长按后误触发K51：如果K50在800ms内长按过，则忽略K51（长按后保护窗口）
            if (now_ms2 - last_k50_longpress_ms_ < 800) {
                ESP_LOGI(TAG, "K51按下但K50在800ms内长按过，可能是长按后的误触发，忽略K51");
                return;
            }
            
            // 如果已有待执行的打断操作，取消旧的定时器
            if (k51_abort_timer_) {
                esp_timer_stop(k51_abort_timer_);
                esp_timer_delete(k51_abort_timer_);
                k51_abort_timer_ = nullptr;
            }
            
            last_k51_click_ms_ = now_ms2;
            k51_pending_abort_ = true;
            
            // 延迟500ms执行打断操作，如果在这期间K50被触发，则取消（增加延迟时间，给K50更多时间取消）
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* self = static_cast<CustomBoard*>(arg);
                    if (!self->k51_pending_abort_) {
                        // 已被取消，不执行
                        return;
                    }
                    self->k51_pending_abort_ = false;
                    self->k51_abort_timer_ = nullptr;
                    
                    // 执行打断操作，与语音唤醒的处理顺序保持一致
                    auto &app = Application::GetInstance();
                    
                    // 确保音频输出已启用
                    auto codec = self->GetAudioCodec();
                    if (codec) {
                        codec->EnableOutput(true);
                    }
                    
                    // 执行打断操作
                    app.AbortSpeaking(kAbortReasonNone);
                    app.StartListening();
                    
                    // 通过 Schedule 异步播放"叮"声提示音，确保在 StartListening 的异步任务之后执行
                    app.Schedule([&app]() {
                        app.GetAudioService().ResetDecoder();
                        app.GetAudioService().PlaySound(Lang::Sounds::P3_WAKE_WORD);
                    }, "K51_PlayWakeSound");
                    
                    app.GetAudioService().EnableVoiceProcessing(true, true);
                    ESP_LOGI("CustomBoard", "K51短按打断已触发,device_state_当前: [%d]", app.GetDeviceState());
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "k51_abort_timer",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &k51_abort_timer_));
            ESP_ERROR_CHECK(esp_timer_start_once(k51_abort_timer_, 500000)); // 500ms延迟
            ESP_LOGI(TAG, "K51按下，延迟500ms执行打断操作（如果K50在期间被触发则取消）");
        });
        
        ESP_LOGI(TAG, "ADC按钮初始化完成");
        
        // 启动ADC调试任务
        // xTaskCreate([](void* param) {
        //     CustomBoard* board = static_cast<CustomBoard*>(param);
        //     while (true) {
        //         int adc_value;
        //         if (adc_oneshot_read(board->adc1_handle_, (adc_channel_t)KEY_ADC_CHANNEL, &adc_value) == ESP_OK) {
        //             float voltage = (adc_value * 3.3f) / 4095.0f;
        //             // ESP_LOGI(TAG, "🔍 ADC调试: 值=%d, 电压=%.3fV", adc_value, voltage);
        //         }
        //         vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒打印一次
        //     }
        // }, "adc_debug", 4096, this, 1, nullptr);
    }

    // 获取数据点缓存
    uint8_t GetBrightness_() {
        auto brightness = 0;
        LWSDataPointManager::GetInstance().GetCachedDataPoint("brightness", brightness);
        return brightness;
    }

    uint8_t GetLightSpeed_() {
        auto light_speed = 0;
        LWSDataPointManager::GetInstance().GetCachedDataPoint("light_speed", light_speed);
        return light_speed;
    }

    uint8_t GetSpeed_() {
        auto speed = 0;
        LWSDataPointManager::GetInstance().GetCachedDataPoint("speed", speed);
        return speed;
    }

    uint8_t GetLightMode_() {
        auto light_mode = 0;
        LWSDataPointManager::GetInstance().GetCachedDataPoint("light_mode", light_mode);
        return light_mode;
    }

    // 将数据点亮度映射为实际应用到LED的亮度：
    // 0 -> 0，1 -> 1，其它取一半（不修改数据点本身）
    uint8_t MapAppliedBrightness_(uint8_t dp_brightness) {
        if (dp_brightness == 0 || dp_brightness == 1) return dp_brightness;
        return static_cast<uint8_t>(dp_brightness / 2);
    }

    void InitializeButtons() {
        // BOOT按键长按
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, " ===== BOOT按键长按 =====");
            ESP_LOGI(TAG, " 按键类型: BOOT按键 (GPIO8)");
            
            // 如果是静默启动状态，长按BOOT按键清除静默标志并重启
            if (silent_startup_from_board_) {
                Settings settings("system", true);
                settings.SetInt("silent_next", 0);
                settings.SetInt("user_wakeup", 1);
                ESP_LOGI(TAG, "充电静默启动状态，长按BOOT清除静默标志并重启");
                esp_restart();
                return;
            }
            
            ESP_LOGI(TAG, " ===== BOOT按键长按 - 立即执行开关机 =====");
            
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
                
                // 刷新一次充电状态并写入 NVS silent_next 与 xingbao 项目一致
                bool is_charging_now = false;
                {
                    PowerManager::GetInstance().CheckBatteryStatusImmediately();
                    vTaskDelay(pdMS_TO_TICKS(20));
                    PowerManager::GetInstance().CheckBatteryStatusImmediately();
                    vTaskDelay(pdMS_TO_TICKS(20));
                    is_charging_now = PowerManager::GetInstance().IsCharging();
                    ESP_LOGI(TAG, "长按键操作前充电状态: %s", is_charging_now ? "充电中" : "未充电");
                }
                {
                    Settings settings("system", true);
                    settings.SetInt("silent_next", is_charging_now ? 1 : 0);
                    if (is_charging_now) {
                        ESP_LOGI(TAG, "充电状态下关机，先保存silent_next=1");
                    } else {
                        ESP_LOGI(TAG, "电池模式下关机，保存silent_next=0");
                    }
                }
                
                // 提前播放音频，与 xingbao 项目保持一致
                ESP_LOGI(TAG, "执行关机操作");
                Application::GetInstance().QuitTalking();
                vTaskDelay(pdMS_TO_TICKS(200));
                auto codec = GetAudioCodec();
                SetBlueLed(false);  // 关闭状态灯
                codec->EnableOutput(true);
                Application::GetInstance().PlaySound(Lang::Sounds::P3_SLEEP);
                need_power_off_ = true;
            } else {
                // 设备关机状态,立即开机
                xTaskCreate([](void* arg) {
                    // 开机：统一走冷启动，触发 Application::Start() -> StartNetwork()
                    ESP_LOGI(TAG, "🔌 设备开机(冷启动)");
                    esp_restart();
                    vTaskDelete(NULL);
                }, "power_on_task", 4028, this, 10, NULL);
            }
        });
        
        // BOOT按键松开
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, " ===== BOOT按键松开 =====");
            if (need_power_off_) {
                need_power_off_ = false;
                // 等待关机提示音播放完成后再关机，行为与 xingbao 项目一致
                xTaskCreate([](void* arg) {
                    auto* board = static_cast<CustomBoard*>(arg);
                    auto& app = Application::GetInstance();
                    ESP_LOGI(TAG, "等待音频播放完成");
                    int wait_count = 0;
                    while (!app.GetAudioService().IsIdle() && wait_count < 50) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        wait_count++;
                    }
                    ESP_LOGI(TAG, "音频播放完成，准备关机");
                    app.SetDeviceState(kDeviceStateIdle);
                    board->PowerOff();
                    vTaskDelete(NULL);
                }, "power_off_task", 4028, this, 10, NULL);
            }
        });
        
        // BOOT按键单击累计计数(600ms 窗口内三击进入配网)
        boot_button_.OnClick([this]() {
            // 静默启动时，三击配网无效
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，三击配网无效");
                return;
            }
            
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
            ESP_LOGI(TAG, "🔋 电源保持引脚已拉高 (GPIO%d)", gpio_num_);
        } else {
            gpio_set_level(gpio_num_, 0);
            ESP_LOGI(TAG, "🔋 GPIO%d 已配置为输出并拉低", gpio_num_);
        }
    }

    void InitializeLWSDataPointManager() {
        
        // 设置 LWSDataPointManager 的回调函数
        LWSDataPointManager::GetInstance().SetCallbacks(
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
            [this]() -> int { 
                int brightness = GetBrightness_();
                static int brightness_print_counter = 0;
                if (++brightness_print_counter >= 30) {
                    brightness_print_counter = 0;
                    ESP_LOGI(TAG, "读取亮度数据点: brightness = %d", brightness);
                }
                return brightness;
            },
            [this](int value) { 
                ESP_LOGI(TAG, "收到亮度数据点设置: brightness = %d", value);
                
                // 静默启动时，不响应数据点设置
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，忽略亮度数据点设置");
                    return;
                }
                
                SetRgbBrightness(value);
                
                // 根据亮度值控制电机
                if (value > 0) {
                    // 亮度>0，开启电机
                    if (!motor_on_) {
                        uint8_t dp_light_speed = GetLightSpeed_();
                        if (dp_light_speed == 0) {
                            LWSDataPointManager::GetInstance().SetDataPointValue("light_speed", 80);
                            dp_light_speed = 80;
                        }
                        motor_control_.SetSpeed(dp_light_speed);
                        motor_control_.Start();
                        motor_on_ = true;
                        ESP_LOGI(TAG, "云端控制：灯开启，启动电机，速度: %d", dp_light_speed);
                    }
                } else {
                    // 亮度=0，关闭电机
                    if (motor_on_) {
                        motor_control_.Stop();
                        motor_on_ = false;
                        ESP_LOGI(TAG, "云端控制：灯关闭，停止电机");
                    }
                    
                    // 亮度为0时，同时将电机速度和模式也设置为0
                    LWSDataPointManager::GetInstance().SetCachedDataPoint("light_speed", 0);
                    LWSDataPointManager::GetInstance().SetCachedDataPoint("light_mode", 0);
                    ESP_LOGI(TAG, "云端控制：灯关闭时，电机速度和模式也设置为0");
                }
                
                // 更新负载补偿状态
                UpdateBatteryLoadComp();
            },
            // 语速回调函数
            [this]() -> int { return GetSpeed_(); },
            [this](int value) { 
                ESP_LOGI(TAG, "语速设置: %d", value);
                // TODO: 实现语速控制逻辑
            },
            // 灯光速度回调函数 - 直接读取数据点，不需要设置回调
            [this]() -> int { 
                int light_speed = GetLightSpeed_();
                static int light_speed_print_counter = 0;
                if (++light_speed_print_counter >= 30) {
                    light_speed_print_counter = 0;
                    ESP_LOGI(TAG, "读取电机速度数据点: light_speed = %d", light_speed);
                }
                return light_speed;
            },
            [this](int value) { 
                ESP_LOGI(TAG, "收到电机速度数据点设置: light_speed = %d", value);
                
                // 静默启动时，不响应数据点设置
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，忽略电机速度数据点设置");
                    return;
                }
                
                // 限制速度范围
                if (value < 0) value = 0;
                if (value > 100) value = 100;

                if (value > 0) {
                    SetMotorSpeed((uint8_t)value);
                    if (!motor_on_) {
                        motor_control_.Start();
                        motor_on_ = true;
                        ESP_LOGI(TAG, "云端控制：电机启动，速度: %d", value);
                        
                        // 如果灯光是关闭的，自动打开灯光以便看到电机效果
                        uint8_t current_brightness = GetBrightness_();
                        if (current_brightness == 0) {
                            LWSDataPointManager::GetInstance().SetDataPointValue("brightness", 80);
                            ESP_LOGI(TAG, "云端控制：电机启动时自动打开灯光，亮度设置为80");
                        }
                    } else {
                        ESP_LOGI(TAG, "云端控制：电机速度更新为: %d", value);
                    }
                } else {
                    SetMotorSpeed(0);
                    if (motor_on_) {
                        motor_control_.Stop();
                        motor_on_ = false;
                        ESP_LOGI(TAG, "云端控制：电机停止");
                    }
                }
                // 同步负载信息
                UpdateBatteryLoadComp();
            },
            // 灯光模式回调函数
            [this]() -> int { 
                int light_mode = GetLightMode_();
                static int light_mode_print_counter = 0;
                if (++light_mode_print_counter >= 30) {
                    light_mode_print_counter = 0;
                    ESP_LOGI(TAG, "读取灯光模式数据点: light_mode = %d", light_mode);
                }
                return light_mode;
            },
            [this](int value) { 
                ESP_LOGI(TAG, "收到灯光模式数据点设置: light_mode = %d", value);
                
                // 静默启动时，不响应数据点设置
                if (silent_startup_from_board_) {
                    ESP_LOGI(TAG, "充电静默启动状态，忽略灯光模式数据点设置");
                    return;
                }
                
                // 如果亮度为0，自动打开灯光以便看到模式效果
                uint8_t current_brightness = GetBrightness_();
                if (current_brightness == 0) {
                    LWSDataPointManager::GetInstance().SetDataPointValue("brightness", 80);
                    ESP_LOGI(TAG, "云端控制：设置灯光模式时自动打开灯光，亮度设置为80");
                }
                
                // 如果电机速度为0，自动设置电机速度以便看到效果
                uint8_t current_speed = GetLightSpeed_();
                if (current_speed == 0) {
                    LWSDataPointManager::GetInstance().SetDataPointValue("light_speed", 100);
                    ESP_LOGI(TAG, "云端控制：设置灯光模式时自动设置电机速度，速度设置为80");
                }
                
                ApplyLedMode_(value);
            }
        );
    }

public:

    // 低电量是否阻止启动
    bool NeedBlockLowBattery() override {
        return true;
    }
    // 是否低电量(基于PowerManager阈值)
    // bool IsLowBattery() const override { return PowerManager::GetInstance().IsLowBattery(); }
    // Set short_press_time to a small non-zero value to enable multiple-click detection reliably
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO, false, 1500, 80), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO), 
                    adc_button_k50_(nullptr), adc_button_k51_(nullptr) {      
        // 记录上电时间
        power_on_time_ = esp_timer_get_time() / 1000; // 转换为毫秒
        ESP_LOGI(TAG, "设备启动,上电时间戳: %lld ms", power_on_time_);
        
        ESP_LOGE(TAG, "CustomBoard ctor 1 start - ERROR level");
        
        // 初始化电源保持引脚
        InitializeGpio(POWER_HOLD_GPIO, true);
        
        // 配置其他必要的GPIO
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO) | (1ULL << RGB_LED_R_GPIO) | (1ULL << RGB_LED_G_GPIO) | (1ULL << RGB_LED_B_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        
        // 初始化LED状态：默认关闭，避免静默启动时短暂点亮
        gpio_set_level(BUILTIN_LED_GPIO, 1);  // active-low: 1=OFF
        gpio_set_level(RGB_LED_R_GPIO, 0);
        gpio_set_level(RGB_LED_G_GPIO, 0);
        gpio_set_level(RGB_LED_B_GPIO, 0);

		// 启动板载指示灯定时器：配网模式下0.5s闪烁，其他状态常亮
		esp_timer_create_args_t led_timer_args = {
			.callback = &CustomBoard::BuiltinLedTimerCallback,
			.arg = this,
			.name = "builtin_led_timer"
		};
		esp_timer_create(&led_timer_args, &builtin_led_timer_);
		esp_timer_start_periodic(builtin_led_timer_, 500000); // 500ms

       
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
        
        // 立即检测一次充电状态
        PowerManager::GetInstance().CheckBatteryStatusImmediately();
        
        // 检查开机复位原因与充电状态，决定是否静默启动
        auto reset_reason = esp_reset_reason();
        Settings sys_settings("system", false);
        int silent_next = sys_settings.GetInt("silent_next", 0);
        int user_wakeup = sys_settings.GetInt("user_wakeup", 0);
        if (user_wakeup == 1) {
            Settings sys_settings_rw("system", true);
            sys_settings_rw.SetInt("user_wakeup", 0);
            silent_startup_from_board_ = false;
        } else if (silent_next == 1) {
            Settings sys_settings_rw("system", true);
            sys_settings_rw.SetInt("silent_next", 0);
            silent_startup_from_board_ = true;
        } else if (reset_reason == ESP_RST_POWERON) {
            bool is_charging = PowerManager::GetInstance().IsCharging();
            if (is_charging) {
                silent_startup_from_board_ = true;
            }
        } else {
            // 异常重启（如看门狗复位、掉电复位等），应该正常启动，不要静默
            ESP_LOGI(TAG, "异常重启（reset_reason: %d），设置正常启动", reset_reason);
            silent_startup_from_board_ = false;
        }
        
        ESP_LOGI(TAG, "silent_startup_from_board_ 最终值: %d", silent_startup_from_board_);
        
        // 静默启动时，禁止点亮状态灯，并禁用省电计时器
        if (silent_startup_from_board_) {
            SetBlueLed(false);  // 关闭状态灯
            SetPowerSaveTimer(false);
        }

        ESP_LOGI(TAG, "Initializing RGB LED and Motor Control...");
        
        rgb_led_.Initialize();
        motor_control_.Initialize();
        ESP_LOGI(TAG, "RGB LED and Motor Control initialized.");
        
        ESP_LOGI(TAG, "Initializing Device State Event...");
        InitializeDeviceStateEvent();
        ESP_LOGI(TAG, "Device State Event initialized.");
        
        // 初始化时检查当前设备状态，如果是 idle 或 sleeping，关闭状态灯
        // 因为如果状态没有变化，状态变化事件不会触发
        // 静默启动时也要关闭状态灯
        if (silent_startup_from_board_) {
            SetBlueLed(false);
            ESP_LOGI(TAG, "静默启动，关闭状态灯");
        } else {
            auto& app = Application::GetInstance();
            DeviceState current_state = app.GetDeviceState();
            if (current_state == kDeviceStateIdle || current_state == kDeviceStateSleeping) {
                SetBlueLed(false);
                ESP_LOGI(TAG, "初始化时检测到休眠状态，关闭状态灯");
            }
        }

        // 开机时不启动电机和RGB灯,等待按键触发
        ESP_LOGI(TAG, "开机完成,等待按键触发电机和灯光");

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            // 只有充电导致的静默启动才忽略唤醒词，异常重启的静默启动允许唤醒词工作
            if (silent_startup_from_board_) {
                ESP_LOGI(TAG, "充电静默启动状态，忽略唤醒词: %s", command.c_str());
                return;
            }
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                ResetWifiConfiguration();
            }
        });

        // 移除开机首次电量相关操作与打印，避免未收敛阶段的误导性输出


        ESP_LOGI(TAG, "Initializing Data Point Manager...");
        InitializeLWSDataPointManager();
        ESP_LOGI(TAG, "Data Point Manager initialized.");

        auto brightness = GetBrightness_();
        auto light_mode = GetLightMode_();
        ESP_LOGI(TAG, "RGB灯光亮度: 数据点=%d, 应用=%d", brightness, MapAppliedBrightness_(brightness));
        ESP_LOGI(TAG, "RGB灯光模式: 数据点=%d", light_mode);
        
        // 设置按键从当前模式的下一个开始
        k51_color_mode_ = light_mode;
        
        // 静默启动时，不恢复RGB灯和电机状态
        if (silent_startup_from_board_) {
            ESP_LOGI(TAG, "静默启动，不恢复RGB灯和电机状态");
            // 确保RGB灯和电机都是关闭的
            StopRgbLightEffect();
            motor_control_.Stop();
            motor_on_ = false;
        } else if (brightness > 0) {
            // 应用数据点中的灯光模式
            ApplyLedMode_(light_mode);
            // 绑定：如果开关开启且亮度>0，则启动电机
            if (!motor_on_) {
                // 检查电机速度数据点，如果为0则设置为80
                uint8_t dp_light_speed = GetLightSpeed_();
                if (dp_light_speed == 0) {
                    LWSDataPointManager::GetInstance().SetDataPointValue("light_speed", 80);
                    ESP_LOGI(TAG, "开机恢复：数据点电机速度为0，已设置为80");
                    dp_light_speed = 80;
                }
                
                motor_control_.SetSpeed(dp_light_speed);
                motor_control_.Start();
                motor_on_ = true;
                ESP_LOGI(TAG, "开机恢复状态：灯开，启动电机，速度: %d", dp_light_speed);
            }
        }

		// 初始化后更新一次负载补偿状态
		UpdateBatteryLoadComp();
    }


    virtual int GetPeriod() override { 
        return 3; 
    }
    virtual int GetMaxFrameNum() override { 
        return 50;
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
        return LWSDataPointManager::GetInstance().GetGizwitsProtocolJson();
    }

    size_t GetDataPointCount() const override {
        return LWSDataPointManager::GetInstance().GetDataPointCount();
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        return LWSDataPointManager::GetInstance().GetDataPointValue(name, value);
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        return LWSDataPointManager::GetInstance().SetDataPointValue(name, value);
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        LWSDataPointManager::GetInstance().GenerateReportData(buffer, buffer_size, data_size);
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        LWSDataPointManager::GetInstance().ProcessDataPointValue(name, value);
    }

    void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) override {
        // LWSDataPointManager 目前没有 binary 数据点，但保持接口一致性
        ESP_LOGW(TAG, "ProcessBinaryDataPointValue called for %s but LWSDataPointManager doesn't support binary data points", name.c_str());
    }

    // RGB LED 控制接口
    void SetRgbColor(uint8_t r, uint8_t g, uint8_t b) {
        rgb_led_.SetColor(r, g, b);
    }
    
    void SetRgbBrightness(uint8_t brightness) {
        uint8_t applied = MapAppliedBrightness_(brightness);
        ESP_LOGI(TAG, "SetRgbBrightness: 数据点=%d, 应用=%d", (int)brightness, (int)applied);
        rgb_led_.SetBrightness(applied);
        if (brightness > 0) {
            if (current_led_mode_ == 0) {
                StartRgbLightEffect();
            } else {
                // 非模式0仅点亮静态颜色/效果，不使能彩虹任务
                rgb_light_on_ = true;
            }
        } else {
            StopRgbLightEffect();
        }
    }
    
    void SetMotorSpeed(uint8_t speed) {
        // 限制速度范围在0-100
        if (speed > 100) speed = 100;
        ESP_LOGI(TAG, "SetMotorSpeed: 数据点=%d", (int)speed);
        motor_control_.SetSpeed(speed);
        // 如果电机正在运行，重新启动以应用新速度
        if (motor_on_ && speed > 0) {
            motor_control_.Start();
            ESP_LOGI(TAG, "电机速度已实时更新为: %d", speed);
        } else if (speed == 0) {
            // 如果速度为0，停止电机
            motor_control_.Stop();
            motor_on_ = false;
            ESP_LOGI(TAG, "电机速度设置为0，已停止电机");
        }
    }

	// HSV 转 RGB，h:0-360, s:0-1, v:0-1；带简单伽马校正，让过渡更顺滑
	static void HsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
		while (h < 0) h += 360.0f;
		while (h >= 360.0f) h -= 360.0f;
		float c = v * s;
		float x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1));
		float m = v - c;
		float r1, g1, b1;
		if (h < 60)      { r1 = c; g1 = x; b1 = 0; }
		else if (h < 120){ r1 = x; g1 = c; b1 = 0; }
		else if (h < 180){ r1 = 0; g1 = c; b1 = x; }
		else if (h < 240){ r1 = 0; g1 = x; b1 = c; }
		else if (h < 300){ r1 = x; g1 = 0; b1 = c; }
		else             { r1 = c; g1 = 0; b1 = x; }
		float rf = r1 + m;
		float gf = g1 + m;
		float bf = b1 + m;
		// 简单伽马校正(gamma≈2.2)
		r = (uint8_t)(powf(fminf(fmaxf(rf, 0.0f), 1.0f), 1.0f/2.2f) * 255.0f + 0.5f);
		g = (uint8_t)(powf(fminf(fmaxf(gf, 0.0f), 1.0f), 1.0f/2.2f) * 255.0f + 0.5f);
		b = (uint8_t)(powf(fminf(fmaxf(bf, 0.0f), 1.0f), 1.0f/2.2f) * 255.0f + 0.5f);
	}

	// sRGB<->Linear 辅助(用于RGB关键帧插值更顺滑)
	static inline float SrgbToLinear(uint8_t c) {
		float cf = c / 255.0f;
		return powf(cf, 2.2f);
	}
	static inline uint8_t LinearToSrgb(float x) {
		float clamped = fminf(fmaxf(x, 0.0f), 1.0f);
		return (uint8_t)(powf(clamped, 1.0f/2.2f) * 255.0f + 0.5f);
	}
    
    
    // 启动RGB灯光效果
    void StartRgbLightEffect() {
        // 打开开关
        rainbow_enabled_ = true;
        rgb_light_on_ = true;
        UpdateBatteryLoadComp();
        // 设置RGB LED亮度（按映射规则应用）
        auto brightness = GetBrightness_();
        rgb_led_.SetBrightness(MapAppliedBrightness_(brightness));
        // 若任务尚未创建，创建一次常驻任务
        if (rgb_task_handle_ != nullptr) {
            return;
        }
        xTaskCreate([](void* param) {
            CustomBoard* board = static_cast<CustomBoard*>(param);
            
            // 全彩渐变彩虹色效果 - 无限循环
            ESP_LOGI(TAG, "开始全彩渐变彩虹色效果");
            
            while (true) {
                // 未使能时休眠等待
                if (!board->rainbow_enabled_) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                // 关键帧表（v_factor 用百分比整数，避免浮点）
                struct Key { uint8_t r, g, b, v_percent; };
                static const Key keys[] = {
                    {255, 182, 193, 100},
                    {255, 105, 180, 100},
                    {255,   0,   0, 100},
                    {255, 165,   0, 100},
                    {255, 255,   0, 115},
                    {144, 238, 144,  90},
                    {  0, 255,   0, 100},
                    {127, 255, 212,  90},
                    {  0, 255, 255, 100},
                    {173, 216, 230,  90},
                    {  0,   0, 255, 100},
                    {216, 191, 216,  90},
                    {255, 182, 193,  85}
                };
                const int nkeys = sizeof(keys)/sizeof(keys[0]);
                const int steps_per_segment = 60;
                const TickType_t step_delay = pdMS_TO_TICKS(32);
                int applied_brightness = board->MapAppliedBrightness_(board->GetBrightness_());
                // 确保低亮度时也能看见（至少30%）
                if (applied_brightness < 30) applied_brightness = 30;
                for (int i = 0; i < nkeys - 1 && board->rainbow_enabled_; ++i) {
                    int r1 = keys[i].r,   g1 = keys[i].g,   b1 = keys[i].b,   v1 = keys[i].v_percent;
                    int r2 = keys[i+1].r, g2 = keys[i+1].g, b2 = keys[i+1].b, v2 = keys[i+1].v_percent;
                    for (int k = 0; k <= steps_per_segment && board->rainbow_enabled_; ++k) {
                        int r = r1 + ((r2 - r1) * k) / steps_per_segment;
                        int g = g1 + ((g2 - g1) * k) / steps_per_segment;
                        int b = b1 + ((b2 - b1) * k) / steps_per_segment;
                        int v = v1 + ((v2 - v1) * k) / steps_per_segment; // 百分比
                        int scaled_r = (r * v * applied_brightness) / (100 * 100);
                        int scaled_g = (g * v * applied_brightness) / (100 * 100);
                        int scaled_b = (b * v * applied_brightness) / (100 * 100);
                        if (scaled_r > 255) scaled_r = 255;
                        if (scaled_g > 255) scaled_g = 255;
                        if (scaled_b > 255) scaled_b = 255;
                        board->SetRgbColor((uint8_t)scaled_r, (uint8_t)scaled_g, (uint8_t)scaled_b);
                        vTaskDelay(step_delay);
                    }
                }
            }
        }, "rgb_light_task", 4096, this, 1, &rgb_task_handle_);
    }
    
    // 停止RGB灯光效果
    void StopRgbLightEffect() {
        if (!rgb_light_on_) {
            return; // 已经关闭
        }
        
        rgb_light_on_ = false;
        rainbow_enabled_ = false; // 任务不断，关闭使能
        SetRgbColor(0, 0, 0);
        UpdateBatteryLoadComp();
        
        ESP_LOGI(TAG, "停止RGB灯光效果");
    }
    
    // 析构函数
    ~CustomBoard() {
        // 停止RGB灯光效果
        StopRgbLightEffect();
        if (rgb_task_handle_ != nullptr) {
            vTaskDelete(rgb_task_handle_);
            rgb_task_handle_ = nullptr;
        }
        
        // 清理 K51 延迟打断定时器
        if (k51_abort_timer_) {
            esp_timer_stop(k51_abort_timer_);
            esp_timer_delete(k51_abort_timer_);
            k51_abort_timer_ = nullptr;
        }
        
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


void* create_board() { 
    ESP_LOGE("CustomBoard", "create_board() called - creating CustomBoard instance");
    return new CustomBoard(); 
}
