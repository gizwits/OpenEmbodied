#include "eye_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>
#include "application.h"
#include "board.h"
#include "font_awesome_symbols.h"
// 包含板级配置文件以访问 WiFi 图标
#include "config.h"

LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

#define TAG "EyeDisplay"

EyeDisplay::EyeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y,
    const lv_img_dsc_t* qrcode_img,
    DisplayFonts fonts)
: EyeDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, fonts) // 委托构造
{
    qrcode_img_ = qrcode_img;
}


EyeDisplay::EyeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y,
                     DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), qrcode_img_(nullptr), fonts_(fonts) {
    width_ = width;
    height_ = height;

    // 创建表情切换队列
    emotion_queue_ = xQueueCreate(EMOTION_QUEUE_SIZE, MAX_EMOTION_LENGTH);
    if (emotion_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create emotion queue");
        return;
    }

    // 创建表情切换任务
    BaseType_t ret = xTaskCreate(
        EmotionTask,
        "emotion_task",
        4096,
        this,
        5,
        &emotion_task_
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create emotion task");
        vQueueDelete(emotion_queue_);
        emotion_queue_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 24;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 10),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
}

EyeDisplay::~EyeDisplay() {
    if (emotion_task_ != nullptr) {
        vTaskDelete(emotion_task_);
        emotion_task_ = nullptr;
    }
    if (emotion_queue_ != nullptr) {
        vQueueDelete(emotion_queue_);
        emotion_queue_ = nullptr;
    }
    if (left_eye_ != nullptr) {
        lv_obj_del(left_eye_);
    }
    if (right_eye_ != nullptr) {
        lv_obj_del(right_eye_);
    }
    if (left_heart_ != nullptr) {
        lv_obj_del(left_heart_);
    }
    if (right_heart_ != nullptr) {
        lv_obj_del(right_heart_);
    }
    if (mouth_) {
        lv_obj_del(mouth_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    if (vertigo_timer_ != nullptr) {
        esp_timer_stop(vertigo_timer_);
        esp_timer_delete(vertigo_timer_);
        vertigo_timer_ = nullptr;
    }
    if (battery_display_timer_ != nullptr) {
        esp_timer_stop(battery_display_timer_);
        esp_timer_delete(battery_display_timer_);
        battery_display_timer_ = nullptr;
    }
    if (battery_charging_update_timer_ != nullptr) {
        esp_timer_stop(battery_charging_update_timer_);
        esp_timer_delete(battery_charging_update_timer_);
        battery_charging_update_timer_ = nullptr;
    }
}

bool EyeDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EyeDisplay::Unlock() {
    lvgl_port_unlock();
}

void EyeDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr || emotion_queue_ == nullptr) {
        return;
    }

    // 检查是否禁用表情切换
    if (emotion_disabled_) {
        return;
    }

    // 将表情字符串复制到队列中
    char* emotion_copy = static_cast<char*>(pvPortMalloc(MAX_EMOTION_LENGTH));
    if (emotion_copy == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for emotion");
        return;
    }

    strncpy(emotion_copy, emotion, MAX_EMOTION_LENGTH - 1);
    emotion_copy[MAX_EMOTION_LENGTH - 1] = '\0';

    if (xQueueSend(emotion_queue_, &emotion_copy, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to send emotion to queue");
        vPortFree(emotion_copy);
    }

    return;
}

void EyeDisplay::EmotionTask(void* arg) {
    EyeDisplay* display = static_cast<EyeDisplay*>(arg);
    char* emotion = nullptr;

    while (true) {
        if (xQueueReceive(display->emotion_queue_, &emotion, portMAX_DELAY) == pdPASS) {
            if (emotion != nullptr) {
                display->ProcessEmotionChange(emotion);
                vPortFree(emotion);
            }
        }
    }
}

void EyeDisplay::ProcessEmotionChange(const char* emotion) {
    if (emotion == nullptr) {
        ESP_LOGW(TAG, "ProcessEmotionChange: emotion is nullptr");
        return;
    }
    
    
    // 测试模式或RGB测试激活时忽略表情切换
    if (test_mode_active_ || rgb_test_active_) {
        return;
    }
    
    // VERTIGO锁定：如果正在锁定且不是VERTIGO请求，直接忽略
    if (vertigo_locked_ && strcmp(emotion, "vertigo") != 0) {
        ESP_LOGW(TAG, "VERTIGO locked, ignore emotion: %s (current_state=%d)", emotion, (int)current_state_);
        // 如果是从视频模式切换回来，强制清除锁定状态
        if (strcmp(emotion, "neutral") == 0) {
            vertigo_locked_ = false;
            if (vertigo_timer_) {
                esp_timer_stop(vertigo_timer_);
            }
        } else {
            return;
        }
    }
    EyeState new_state = current_state_;
    
    if (strcmp(emotion, "neutral") == 0) {
        new_state = EyeState::IDLE;
    } else if (strcmp(emotion, "happy") == 0) {
        new_state = EyeState::HAPPY;
    } else if (strcmp(emotion, "laughing") == 0) {
        new_state = EyeState::LOVING;
    } else if (strcmp(emotion, "sad") == 0) {
        new_state = EyeState::SAD;
    } else if (strcmp(emotion, "angry") == 0) {
        new_state = EyeState::ANGRY;
    } else if (strcmp(emotion, "crying") == 0) {
        new_state = EyeState::SAD;
    } else if (strcmp(emotion, "loving") == 0) {
        new_state = EyeState::LOVING;
    } else if (strcmp(emotion, "embarrassed") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "surprised") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "shocked") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "thinking") == 0) {
        new_state = EyeState::THINKING;
    } else if (strcmp(emotion, "winking") == 0) {
        new_state = EyeState::WINKING;
    } else if (strcmp(emotion, "cool") == 0) {
        new_state = EyeState::HAPPY;
    } else if (strcmp(emotion, "relaxed") == 0) {
        new_state = EyeState::HAPPY;
    } else if (strcmp(emotion, "delicious") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "kissy") == 0) {
        new_state = EyeState::LOVING;
    } else if (strcmp(emotion, "confident") == 0) {
        new_state = EyeState::HAPPY;
    } else if (strcmp(emotion, "sleepy") == 0) {
        new_state = EyeState::SLEEPING;
    } else if (strcmp(emotion, "silly") == 0) {
        new_state = EyeState::SILLY;
    } else if (strcmp(emotion, "confused") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "vertigo") == 0) {
        new_state = EyeState::VERTIGO;
    }

    // 使用锁保护状态切换
    DisplayLockGuard lock(this);
    
    // 如果状态相同，仍然需要确保眼睛可见并重新初始化（用于从视频模式切换回来时）
    if (new_state == current_state_) {
        ESP_LOGI(TAG, "ProcessEmotionChange: same state (%d), reinitializing components", (int)new_state);
        // 确保眼睛对象和容器可见
        if (left_eye_ != nullptr && right_eye_ != nullptr) {
            lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "Cleared HIDDEN flag for eyes");
            // 确保容器的父对象也可见
            lv_obj_t* container = lv_obj_get_parent(left_eye_);
            if (container != nullptr) {
                lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
                // 确保容器移到前景
                lv_obj_move_foreground(container);
                ESP_LOGI(TAG, "Cleared HIDDEN flag for container and moved to foreground");
            } else {
                ESP_LOGW(TAG, "Container is nullptr");
            }
        } else {
            ESP_LOGW(TAG, "Eyes are nullptr: left=%p, right=%p", left_eye_, right_eye_);
        }
        
        // 清理可能存在的组件（无论当前状态是什么），确保重新创建
        if (left_heart_) {
            lv_anim_del(left_heart_, nullptr);
            lv_obj_del(left_heart_);
            left_heart_ = nullptr;
        }
        if (right_heart_) {
            lv_anim_del(right_heart_, nullptr);
            lv_obj_del(right_heart_);
            right_heart_ = nullptr;
        }
        if (mouth_) {
            lv_anim_del(mouth_, nullptr);
            lv_obj_del(mouth_);
            mouth_ = nullptr;
        }
        if (right_tear_) {
            lv_obj_del(right_tear_);
            right_tear_ = nullptr;
        }
        if (left_hand_) {
            lv_anim_del(left_hand_, nullptr);
            lv_obj_del(left_hand_);
            left_hand_ = nullptr;
        }
        if (right_hand_) {
            lv_anim_del(right_hand_, nullptr);
            lv_obj_del(right_hand_);
            right_hand_ = nullptr;
        }
        if (zzz1_) {
            lv_obj_del(zzz1_);
            zzz1_ = nullptr;
        }
        if (zzz2_) {
            lv_obj_del(zzz2_);
            zzz2_ = nullptr;
        }
        if (zzz3_) {
            lv_obj_del(zzz3_);
            zzz3_ = nullptr;
        }
        
        // 重置眼睛状态
        if (left_eye_ != nullptr) {
            lv_obj_set_style_transform_angle(left_eye_, 0, 0);
        }
        if (right_eye_ != nullptr) {
            lv_obj_set_style_transform_angle(right_eye_, 0, 0);
        }
        if (new_state != EyeState::SLEEPING) {
            if (left_eye_ != nullptr) {
                lv_obj_set_size(left_eye_, 40, 80);
                lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
            }
            if (right_eye_ != nullptr) {
                lv_obj_set_size(right_eye_, 40, 80);
                lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
            }
        }
        
        // 停止当前动画
        lv_anim_del(left_eye_, nullptr);
        lv_anim_del(right_eye_, nullptr);
        
        // 对于所有状态，都需要重新初始化动画和组件（从视频模式切换回来时）
        switch (new_state) {
            case EyeState::SURPRISED:
            case EyeState::IDLE:
                StartIdleAnimation();
                break;
            case EyeState::RELAXED:
            case EyeState::CONFIDENT:
            case EyeState::COOL:
            case EyeState::WINKING:
            case EyeState::HAPPY:
                StartHappyAnimation();
                break;
            case EyeState::ANGRY:
                StartAngryAnimation();
                break;
            case EyeState::CRYING:
            case EyeState::SAD:
                StartSadAnimation();
                break;
            case EyeState::KISSY:
            case EyeState::LAUGHING:
            case EyeState::LOVING:
                StartLovingAnimation();
                break;
            case EyeState::CONFUSED:
            case EyeState::DELICIOUS:
            case EyeState::EMBARRASSED:
            case EyeState::THINKING:
                StartThinkingAnimation();
                break;
            case EyeState::SHOCKED:
                StartShockedAnimation();
                break;
            case EyeState::SLEEPING:
                StartSleepingAnimation();
                break;
            case EyeState::SILLY:
                StartSillyAnimation();
                break;
            case EyeState::VERTIGO:
                StartVertigoAnimation();
                break;
        }
        
        // 如果正在充电，确保充电时的电量圆环移到最前面（视频模式下也要保持显示）
        if (charging_indicator_showing_ && charging_battery_arc_ != nullptr) {
            // 检查对象是否仍然有效
            if (lv_obj_is_valid(charging_battery_arc_)) {
                lv_obj_move_foreground(charging_battery_arc_);
                lv_obj_clear_flag(charging_battery_arc_, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG, "ProcessEmotionChange: 确保充电时的电量圆环在最前面");
            } else {
                ESP_LOGW(TAG, "ProcessEmotionChange: 充电圆环对象已失效，重新创建");
                charging_battery_arc_ = nullptr;
                charging_indicator_showing_ = false;
                // 如果正在充电，重新显示圆环
                int battery_level = 0;
                bool charging = false;
                bool discharging = false;
                Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
                if (charging) {
                    ShowBatteryIndicatorForCharging(battery_level);  // 传递实际电量值
                }
            }
        }
        
        // 处理VERTIGO和LOVING的锁定逻辑
        // 注意：从视频模式切换回来时，不应该重新锁定（除非是VERTIGO状态）
        if (new_state == EyeState::VERTIGO) {
            vertigo_locked_ = true;
            ESP_LOGI(TAG, "VERTIGO state: setting vertigo_locked=true");
        } else if (new_state == EyeState::LOVING) {
            // LOVING状态也需要锁定，但只在非视频模式切换时
            vertigo_locked_ = true;
            ESP_LOGI(TAG, "LOVING state: setting vertigo_locked=true");
        } else {
            // 其他状态清除锁定
            vertigo_locked_ = false;
            ESP_LOGI(TAG, "Non-locking state: clearing vertigo_locked");
        }
        if (new_state == EyeState::VERTIGO || new_state == EyeState::LOVING || new_state == EyeState::THINKING) {
            if (new_state == EyeState::THINKING) {
                vertigo_unlock_time_ = esp_timer_get_time() + 2000000LL; // 2秒后解锁
            } else {
                vertigo_unlock_time_ = esp_timer_get_time() + 5000000LL; // 5秒后解锁
            }
            if (vertigo_timer_ == nullptr) {
                esp_timer_create_args_t timer_args = {
                    .callback = [](void* arg) {
                        EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                        self->vertigo_locked_ = false;
                        ESP_LOGI(TAG, "VERTIGO unlock, auto switch to idle");
                        self->SetEmotion("neutral");
                        if (Application::GetInstance().GetDeviceState() == DeviceState::kDeviceStateIdle) {
                            self->SetEmotion("sleepy");
                        } else {
                            self->SetEmotion("neutral");
                        }
                    },
                    .arg = this,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "vertigo_timer"
                };
                esp_timer_create(&timer_args, &vertigo_timer_);
            }
            esp_timer_stop(vertigo_timer_);
            if (new_state == EyeState::THINKING) {
                esp_timer_start_once(vertigo_timer_, 2000000); // 2秒
            } else {
                esp_timer_start_once(vertigo_timer_, 5000000); // 5秒
            }
        } else {
            vertigo_locked_ = false;
            if (vertigo_timer_) esp_timer_stop(vertigo_timer_);
        }
        
        return;
    }

    ESP_LOGI(TAG, "ProcessEmotionChange: different state (%d -> %d), cleaning up and reinitializing", 
             (int)current_state_, (int)new_state);
    
    // 如果新状态不是睡眠状态，删除 zzz 标签（无论当前状态是什么）
    if (new_state != EyeState::SLEEPING) {
        if (zzz1_) {
            lv_obj_del(zzz1_);
            zzz1_ = nullptr;
        }
        if (zzz2_) {
            lv_obj_del(zzz2_);
            zzz2_ = nullptr;
        }
        if (zzz3_) {
            lv_obj_del(zzz3_);
            zzz3_ = nullptr;
        }
    }

    // 清理可能存在的爱心对象（无论从什么状态切换）
    if (left_heart_) {
        lv_anim_del(left_heart_, nullptr);  // 停止左眼爱心动画
        lv_obj_del(left_heart_);
        left_heart_ = nullptr;
    }
    if (right_heart_) {
        lv_anim_del(right_heart_, nullptr);  // 停止右眼爱心动画
        lv_obj_del(right_heart_);
        right_heart_ = nullptr;
    }

    // 清理可能存在的嘴巴对象（无论从什么状态切换）
    if (mouth_) {
        lv_anim_del(mouth_, nullptr);  // 停止嘴巴动画
        lv_obj_del(mouth_);
        mouth_ = nullptr;
    }
    // 清理可能存在的右眼眼泪对象
    if (right_tear_) {
        lv_obj_del(right_tear_);
        right_tear_ = nullptr;
    }

    // 清理可能存在的手部对象（无论从什么状态切换）
    if (left_hand_) {
        lv_anim_del(left_hand_, nullptr);  // 停止左手动画
        lv_obj_del(left_hand_);
        left_hand_ = nullptr;
    }
    if (right_hand_) {
        lv_anim_del(right_hand_, nullptr);  // 停止右手动画
        lv_obj_del(right_hand_);
        right_hand_ = nullptr;
    }

    // 确保眼睛对象可见（如果它们存在）
    if (left_eye_ != nullptr) {
        lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_eye_ != nullptr) {
        lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 重置眼睛旋转角度（特别是从生气状态切换出来时）
    if (left_eye_ != nullptr) {
        lv_obj_set_style_transform_angle(left_eye_, 0, 0);
    }
    if (right_eye_ != nullptr) {
        lv_obj_set_style_transform_angle(right_eye_, 0, 0);
    }
    
    // 重置眼睛尺寸为默认值（除了睡眠状态）
    if (new_state != EyeState::SLEEPING) {
        if (left_eye_ != nullptr) {
            lv_obj_set_size(left_eye_, 40, 80);
            lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
        }
        if (right_eye_ != nullptr) {
            lv_obj_set_size(right_eye_, 40, 80);
            lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
        }
    }

    current_state_ = new_state;

    // 停止当前动画
    lv_anim_del(left_eye_, nullptr);
    lv_anim_del(right_eye_, nullptr);

    // 启动对应状态的表情动画
    switch (current_state_) {
        case EyeState::SURPRISED:
        case EyeState::IDLE:
            StartIdleAnimation();
            break;
        case EyeState::RELAXED:
        case EyeState::CONFIDENT:
        case EyeState::COOL:
        case EyeState::WINKING:
        case EyeState::HAPPY:
            StartHappyAnimation();
            break;
        case EyeState::ANGRY:
            StartAngryAnimation();
            break;
        case EyeState::CRYING:
        case EyeState::SAD:
            StartSadAnimation();
            break;
        case EyeState::KISSY:
        case EyeState::LAUGHING:
        case EyeState::LOVING:
            StartLovingAnimation();
            break;
        case EyeState::CONFUSED:
        case EyeState::DELICIOUS:
        case EyeState::EMBARRASSED:
        case EyeState::THINKING:
             StartThinkingAnimation();
             break;
        case EyeState::SHOCKED:
            StartShockedAnimation();
            break;
        case EyeState::SLEEPING:
            StartSleepingAnimation();
            break;
        case EyeState::SILLY:
            StartSillyAnimation();
            break;
        case EyeState::VERTIGO:
            StartVertigoAnimation();
            break;
        default:
            ESP_LOGW(TAG, "Unknown state %d, no animation started", (int)current_state_);
            break;
    }

    // 如果正在充电，确保充电时的电量圆环移到最前面（视频模式下也要保持显示）
    if (charging_indicator_showing_ && charging_battery_arc_ != nullptr) {
        // 检查对象是否仍然有效
        if (lv_obj_is_valid(charging_battery_arc_)) {
            lv_obj_move_foreground(charging_battery_arc_);
            lv_obj_clear_flag(charging_battery_arc_, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "ProcessEmotionChange: 确保充电时的电量圆环在最前面");
        } else {
            ESP_LOGW(TAG, "ProcessEmotionChange: 充电圆环对象已失效，重新创建");
            charging_battery_arc_ = nullptr;
            charging_indicator_showing_ = false;
            // 如果正在充电，重新显示圆环
            int battery_level = 0;
            bool charging = false;
            bool discharging = false;
            Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
            if (charging) {
                ShowBatteryIndicatorForCharging(battery_level);  // 传递实际电量值
            }
        }
    }

    if (current_state_ == EyeState::VERTIGO || current_state_ == EyeState::LOVING) {
        // 眩晕动画需要锁定
        vertigo_locked_ = true;
    }
    // VERTIGO LOVING THINKING 需要自动取消
    if (current_state_ == EyeState::VERTIGO || current_state_ == EyeState::LOVING || current_state_ == EyeState::THINKING) {
        if (current_state_ == EyeState::THINKING) {
            vertigo_unlock_time_ = esp_timer_get_time() + 2000000LL; // 2秒后解锁
        } else {
            vertigo_unlock_time_ = esp_timer_get_time() + 5000000LL; // 5秒后解锁
        }
        if (vertigo_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                    self->vertigo_locked_ = false;
                    ESP_LOGI(TAG, "VERTIGO unlock, auto switch to idle");
                    self->SetEmotion("neutral");
                    if (Application::GetInstance().GetDeviceState() == DeviceState::kDeviceStateIdle) {
                        self->SetEmotion("sleepy");
                    } else {
                        self->SetEmotion("neutral");
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "vertigo_timer"
            };
            esp_timer_create(&timer_args, &vertigo_timer_);
        }
        esp_timer_stop(vertigo_timer_);

        if (current_state_ == EyeState::THINKING) {
            esp_timer_start_once(vertigo_timer_, 2000000); // 2秒
        } else {
            esp_timer_start_once(vertigo_timer_, 5000000); // 5秒
        }
    } else {
        // 非VERTIGO状态，确保锁定解除
        vertigo_locked_ = false;
        if (vertigo_timer_) esp_timer_stop(vertigo_timer_);
    }
}

void EyeDisplay::StartIdleAnimation() {
    // 检查眼睛对象是否存在
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        ESP_LOGW(TAG, "StartIdleAnimation: eyes not initialized");
        return;
    }
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
    lv_anim_set_time(&left_anim_, 600);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 600);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 80);
    lv_anim_set_time(&right_anim_, 600);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 600);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartHappyAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
    lv_anim_set_time(&left_anim_, 700);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 700);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 80);
    lv_anim_set_time(&right_anim_, 700);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 700);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);

    // 创建嘴巴图片对象
    mouth_ = lv_img_create(lv_scr_act());
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 52 - DISPLAY_VERTICAL_OFFSET);  // 居中，距离底部52像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 52 - DISPLAY_VERTICAL_OFFSET, height_ - 62 - DISPLAY_VERTICAL_OFFSET);  // 在-52到-62像素之间移动
    lv_anim_set_time(&mouth_anim_, 1000);
    lv_anim_set_delay(&mouth_anim_, 0);
    lv_anim_set_exec_cb(&mouth_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, 1000);
    lv_anim_set_playback_delay(&mouth_anim_, 0);
    lv_anim_start(&mouth_anim_);
}

void EyeDisplay::StartSadAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 设置眼睛为水平长条
    lv_obj_set_size(left_eye_, 60, 20);
    lv_obj_set_size(right_eye_, 60, 20);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    

    // 创建嘴巴图片对象
    mouth_ = lv_img_create(lv_scr_act());
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 52 - DISPLAY_VERTICAL_OFFSET);  // 居中，距离底部52像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度
    
    // 旋转嘴巴180度，使其变成向上的箭头
    lv_obj_set_style_transform_angle(mouth_, 1800, 0);  // 180度 = 1800 * 0.1度
    
    // 重新调整位置，确保旋转后仍然居中
    lv_obj_set_pos(mouth_, (width_ + 32) / 2, height_ - 32 - DISPLAY_VERTICAL_OFFSET);

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 32 - DISPLAY_VERTICAL_OFFSET, height_ - 42 - DISPLAY_VERTICAL_OFFSET); 
    lv_anim_set_time(&mouth_anim_, 1200);
    lv_anim_set_delay(&mouth_anim_, 0);
    lv_anim_set_exec_cb(&mouth_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, 1200);
    lv_anim_set_playback_delay(&mouth_anim_, 0);
    lv_anim_start(&mouth_anim_);

    // 创建右眼眼泪（椭圆）
    right_tear_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(right_tear_, 12, 20); // 椭圆形状
    lv_obj_set_style_radius(right_tear_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear_, lv_color_hex(EYE_COLOR), 0); // 青色
    lv_obj_set_style_border_width(right_tear_, 0, 0);
    lv_obj_set_style_border_side(right_tear_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_tear_, 0, 0);
    lv_obj_set_style_shadow_width(right_tear_, 0, 0);
    lv_obj_set_style_outline_width(right_tear_, 0, 0);

    // 位置在屏幕右侧，眼睛下方
    lv_obj_set_pos(right_tear_, width_ - 60, height_ / 2 + 20 - DISPLAY_VERTICAL_OFFSET);
    // 添加下落动画
    static lv_anim_t tear_anim;
    lv_anim_init(&tear_anim);
    lv_anim_set_var(&tear_anim, right_tear_);
    lv_anim_set_values(&tear_anim, height_ / 2 + 40 - DISPLAY_VERTICAL_OFFSET, height_ / 2 + 20 - DISPLAY_VERTICAL_OFFSET);
    lv_anim_set_time(&tear_anim, 1000);
    lv_anim_set_repeat_count(&tear_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&tear_anim, 1000);
    lv_anim_set_exec_cb(&tear_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&tear_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&tear_anim);
}


void EyeDisplay::StartVertigoAnimation() {
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 创建左眼爱心图片
    lv_obj_t* left_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(left_heart, &spiral_img_64);
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, 0, -DISPLAY_VERTICAL_OFFSET);  // 左眼位置，距离左边缘40像素
    
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &spiral_img_64);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, -0, -DISPLAY_VERTICAL_OFFSET);  // 右眼位置，距离右边缘40像素
    
    // 缩小图片
    lv_img_set_zoom(left_heart, 128);
    lv_img_set_zoom(right_heart, 128);

    // 保存爱心对象指针，以便在状态切换时清理
    left_heart_ = left_heart;
    right_heart_ = right_heart;
    
    // 为左眼爱心添加循环旋转动画
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_heart);
    lv_anim_set_values(&left_anim_, 0, -3600);  // 从0度旋转到3600度（10圈）
    lv_anim_set_time(&left_anim_, 1000);  // 更快的旋转速度
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&left_anim_);
    
    // 为右眼爱心添加循环旋转动画
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_heart);
    lv_anim_set_values(&right_anim_, 0, 3600);  // 从0度旋转到-3600度（逆时针10圈）
    lv_anim_set_time(&right_anim_, 1000);  // 更快的旋转速度
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&right_anim_);
}


void EyeDisplay::StartLovingAnimation() {
    // 隐藏原来的圆形眼睛
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 创建左眼爱心图片
    lv_obj_t* left_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(left_heart, &hart_img);
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, 0, -DISPLAY_VERTICAL_OFFSET);  // 左眼位置，距离左边缘40像素
    
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &hart_img);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, -0, -DISPLAY_VERTICAL_OFFSET);  // 右眼位置，距离右边缘40像素
    
    // 保存爱心对象指针，以便在状态切换时清理
    left_heart_ = left_heart;
    right_heart_ = right_heart;

    // 左眼爱心放大缩小动画
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_heart);
    lv_anim_set_values(&left_anim_, 74, 128);  // 从100%放大到200%
    lv_anim_set_time(&left_anim_, 500);         // 放大时间
    lv_anim_set_playback_time(&left_anim_, 500);// 缩小时间
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_overshoot);
    lv_anim_start(&left_anim_);

    // 右眼爱心放大缩小动画
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_heart);
    lv_anim_set_values(&right_anim_, 74, 128);  // 从100%放大到200%
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_overshoot);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartSleepingAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 设置眼睛为水平长条
    lv_obj_set_size(left_eye_, 60, 20);
    lv_obj_set_size(right_eye_, 60, 20);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);

    // 先清理可能存在的旧zzz对象，避免重复创建
    if (zzz1_) {
        lv_obj_del(zzz1_);
        zzz1_ = nullptr;
    }
    if (zzz2_) {
        lv_obj_del(zzz2_);
        zzz2_ = nullptr;
    }
    if (zzz3_) {
        lv_obj_del(zzz3_);
        zzz3_ = nullptr;
    }

    // 创建三个 z 标签
    zzz1_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz1_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz1_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz1_, "z");
    lv_obj_align(zzz1_, LV_ALIGN_TOP_MID, -40, 50 - DISPLAY_VERTICAL_OFFSET);  // 调整垂直位置到 50
    lv_obj_set_style_text_letter_space(zzz1_, 2, 0);  // 增加字间距

    zzz2_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz2_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz2_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz2_, "z");
    lv_obj_align(zzz2_, LV_ALIGN_TOP_MID, 0, 40 - DISPLAY_VERTICAL_OFFSET);  // 调整垂直位置到 40
    lv_obj_set_style_text_letter_space(zzz2_, 2, 0);  // 增加字间距

    zzz3_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz3_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz3_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz3_, "z");
    lv_obj_align(zzz3_, LV_ALIGN_TOP_MID, 40, 30 - DISPLAY_VERTICAL_OFFSET);  // 调整垂直位置到 30
    lv_obj_set_style_text_letter_space(zzz3_, 2, 0);  // 增加字间距
}

void EyeDisplay::DeleteZzzObjects() {
    // 删除已知的zzz对象
    if (zzz1_) {
        lv_obj_del(zzz1_);
        zzz1_ = nullptr;
    }
    if (zzz2_) {
        lv_obj_del(zzz2_);
        zzz2_ = nullptr;
    }
    if (zzz3_) {
        lv_obj_del(zzz3_);
        zzz3_ = nullptr;
    }
    
    // 额外检查：遍历屏幕的所有子对象，查找并删除所有包含"z"文本的label对象
    // 这可以确保即使zzz对象指针丢失，也能删除它们
    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
        for (int32_t i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (child != nullptr && lv_obj_check_type(child, &lv_label_class)) {
                const char* text = lv_label_get_text(child);
                if (text != nullptr && strcmp(text, "z") == 0) {
                    lv_obj_del(child);
                }
            }
        }
    }
}

void EyeDisplay::StartShockedAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
    lv_anim_set_time(&left_anim_, 800);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 800);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 80);
    lv_anim_set_time(&right_anim_, 800);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 800);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
    
    // 创建圆形嘴巴
    mouth_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(mouth_, 30, 30);  // 圆形嘴巴
    lv_obj_set_style_radius(mouth_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_pad_all(mouth_, 0, 0);
    lv_obj_set_style_shadow_width(mouth_, 0, 0);
    lv_obj_set_style_outline_width(mouth_, 0, 0);
    lv_obj_set_pos(mouth_, (width_ - 30) / 2, height_ - 70 - DISPLAY_VERTICAL_OFFSET);  // 居中显示
    
    // 为嘴巴添加大小动画，模拟震惊的效果
    static lv_anim_t mouth_size_anim;
    lv_anim_init(&mouth_size_anim);
    lv_anim_set_var(&mouth_size_anim, mouth_);
    lv_anim_set_values(&mouth_size_anim, 20, 35);  // 从小到大变化
    lv_anim_set_time(&mouth_size_anim, 800);
    lv_anim_set_exec_cb(&mouth_size_anim, [](void* obj, int32_t value) {
        lv_obj_set_size((lv_obj_t*)obj, value, value);
        // 重新居中
        lv_obj_set_x((lv_obj_t*)obj, (240 - value) / 2);
    });
    lv_anim_set_path_cb(&mouth_size_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_size_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_size_anim, 800);
    lv_anim_start(&mouth_size_anim);
}

void EyeDisplay::StartSillyAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
    lv_anim_set_time(&left_anim_, 1000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 80);
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartAngryAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 设置眼睛为倾斜的形状（内高外低）
    // 左眼：右高左低
    lv_obj_set_size(left_eye_, 50, 60);
    lv_obj_set_style_radius(left_eye_, 15, 0);
    lv_obj_set_style_transform_angle(left_eye_, -150, 0);  // -15度倾斜
    
    // 右眼：左高右低  
    lv_obj_set_size(right_eye_, 50, 60);
    lv_obj_set_style_radius(right_eye_, 15, 0);
    lv_obj_set_style_transform_angle(right_eye_, 150, 0);  // 15度倾斜
    
    // 眼睛微微跳动的动画
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 55, 65);
    lv_anim_set_time(&left_anim_, 500);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 500);
    lv_anim_start(&left_anim_);
    
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 55, 65);
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_start(&right_anim_);
    
    // 创建生气的嘴巴（紧闭的横线）
    mouth_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(mouth_, 40, 4);  // 横线形状
    lv_obj_set_style_radius(mouth_, 2, 0);
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_pad_all(mouth_, 0, 0);
    lv_obj_set_style_shadow_width(mouth_, 0, 0);
    lv_obj_set_style_outline_width(mouth_, 0, 0);
    lv_obj_set_pos(mouth_, (width_ - 40) / 2, height_ - 60 - DISPLAY_VERTICAL_OFFSET);
    
    // 嘴巴微微抖动的动画
    static lv_anim_t mouth_width_anim;
    lv_anim_init(&mouth_width_anim);
    lv_anim_set_var(&mouth_width_anim, mouth_);
    lv_anim_set_values(&mouth_width_anim, 35, 45);  // 宽度变化
    lv_anim_set_time(&mouth_width_anim, 400);
    lv_anim_set_exec_cb(&mouth_width_anim, [](void* obj, int32_t value) {
        lv_obj_set_width((lv_obj_t*)obj, value);
        // 重新居中
        lv_obj_set_x((lv_obj_t*)obj, (240 - value) / 2);
    });
    lv_anim_set_path_cb(&mouth_width_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_width_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_width_anim, 400);
    lv_anim_start(&mouth_width_anim);
}

void EyeDisplay::StartThinkingAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 眼睛动画 - 思考时的眨眼效果
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 60);
    lv_anim_set_time(&left_anim_, 2000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 2000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 60);
    lv_anim_set_time(&right_anim_, 2000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 2000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);

    // 创建嘴巴图片对象
    mouth_ = lv_img_create(lv_scr_act());
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 90 - DISPLAY_VERTICAL_OFFSET);
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);


    // 创建左手图片
    left_hand_ = lv_img_create(lv_scr_act());
    lv_img_set_src(left_hand_, &hand_img);
    lv_obj_set_style_img_recolor(left_hand_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(left_hand_, LV_OPA_COVER, 0);
    lv_obj_set_pos(left_hand_, 20, height_ - 50 - DISPLAY_VERTICAL_OFFSET);

    // 创建右手图片
    right_hand_ = lv_img_create(lv_scr_act());
    lv_img_set_src(right_hand_, &hand_right_img);
    lv_obj_set_style_img_recolor(right_hand_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(right_hand_, LV_OPA_COVER, 0);
    // 设置右手位置
    lv_obj_set_pos(right_hand_, width_ - 74, height_ - 50 - DISPLAY_VERTICAL_OFFSET);
    // 左手左右移动动画
    static lv_anim_t left_hand_anim;
    lv_anim_init(&left_hand_anim);
    lv_anim_set_var(&left_hand_anim, left_hand_);
    lv_anim_set_values(&left_hand_anim, 40, 60);
    lv_anim_set_time(&left_hand_anim, 1000);
    lv_anim_set_delay(&left_hand_anim, 0);
    lv_anim_set_exec_cb(&left_hand_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&left_hand_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_hand_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_hand_anim, 1000);
    lv_anim_set_playback_delay(&left_hand_anim, 0);
    lv_anim_start(&left_hand_anim);

    // 右手左右移动动画（与左手相反）
    static lv_anim_t right_hand_anim;
    lv_anim_init(&right_hand_anim);
    lv_anim_set_var(&right_hand_anim, right_hand_);
    lv_anim_set_values(&right_hand_anim, width_ - 74, width_ - 100);
    lv_anim_set_time(&right_hand_anim, 1000);
    lv_anim_set_delay(&right_hand_anim, 0);
    lv_anim_set_exec_cb(&right_hand_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&right_hand_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_hand_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_hand_anim, 1000);
    lv_anim_set_playback_delay(&right_hand_anim, 0);
    lv_anim_start(&right_hand_anim);
}

void EyeDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Create a container for eyes
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    // 设置容器的顶部内边距来实现向上偏移
    lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 10, 0);

    // Create left eye
    left_eye_ = lv_obj_create(container);
    lv_obj_set_size(left_eye_, 40, 80);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_eye_, 0, 0);
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);
    lv_obj_set_style_outline_width(left_eye_, 0, 0);

    // Create right eye
    right_eye_ = lv_obj_create(container);
    lv_obj_set_size(right_eye_, 40, 80);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_eye_, 0, 0);
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);
    lv_obj_set_style_outline_width(right_eye_, 0, 0);

    // 初始化时隐藏眼睛对象和容器，避免在视频播放前显示默认表情
    // 视频播放会在 TriggerEmotion 时通过 VideoPlayer 显示
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "SetupUI: 初始化时隐藏眼睛对象，等待视频播放");

    // 禁用默认的待机动画，改用视频播放，节省内存
    // 不再调用 StartIdleAnimation()，等待后续的 SetEmotion 调用来触发视频播放
    // StartIdleAnimation();  // 已注释，统一使用视频播放
}

void EyeDisplay::TestNextEmotion() {
    // 定义所有表情的字符串数组，按EyeState枚举的顺序
    static const char* emotions[] = {
        "neutral",      // IDLE
        "happy",        // HAPPY
        "laughing",     // LAUGHING
        "sad",          // SAD
        "angry",        // ANGRY
        "crying",       // CRYING
        "loving",       // LOVING
        "embarrassed",  // EMBARRASSED
        "surprised",    // SURPRISED
        "shocked",      // SHOCKED
        "thinking",     // THINKING
        "winking",      // WINKING
        "cool",         // COOL
        "relaxed",      // RELAXED
        "delicious",    // DELICIOUS
        "kissy",        // KISSY
        "confident",    // CONFIDENT
        "sleepy",       // SLEEPING
        "silly",        // SILLY
        "confused",     // CONFUSED
        "vertigo"       // VERTIGO
    };
    
    static const size_t emotion_count = sizeof(emotions) / sizeof(emotions[0]);
    static size_t current_index = 0;
    
    // 获取当前表情的字符串
    const char* emotion = emotions[current_index];
    
    // 输出当前表情信息到日志
    ESP_LOGI(TAG, "Testing emotion %zu/%zu: %s", current_index + 1, emotion_count, emotion);
    
    // 设置表情
    SetEmotion(emotion);
    
    // 移动到下一个表情
    current_index = (current_index + 1) % emotion_count;
} 

void EyeDisplay::EnterWifiConfig() {
    ESP_LOGI(TAG, "EnterWifiConfig");
    
    // 禁用表情切换
    emotion_disabled_ = true;
    
    // 隐藏充电时的电量圆环（如果存在）
    if (charging_indicator_showing_ && charging_battery_arc_ != nullptr) {
        DisplayLockGuard lock(this);
        if (charging_battery_arc_ != nullptr) {
            lv_obj_del(charging_battery_arc_);
            charging_battery_arc_ = nullptr;
        }
        charging_indicator_showing_ = false;
        if (battery_charging_update_timer_ != nullptr) {
            esp_timer_stop(battery_charging_update_timer_);
        }
    }
    
    if (qrcode_img_) {
        ESP_LOGI(TAG, "EnterWifiConfig qrcode_img_ is not null");
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        if (screen == nullptr) {
            ESP_LOGE(TAG, "EnterWifiConfig: screen is nullptr");
            return;
        }
        
        // 先逐个删除子对象，避免访问已删除的对象
        // 注意：删除对象前先检查对象是否有效，避免访问已删除的对象
        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
        ESP_LOGI(TAG, "EnterWifiConfig: 删除 %u 个子对象", child_cnt);
        for (int32_t i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (child != nullptr && lv_obj_is_valid(child)) {
                lv_obj_del(child);
            }
        }
        
        // 强制刷新LVGL，确保删除操作完成
        lv_refr_now(nullptr);
        
        // 等待更长时间确保删除完成和LVGL稳定
        // 同时给视频任务时间完全退出（DeviceStateEventManager回调中已经停止，但这里再等待一下确保）
        // 增加等待时间，确保视频任务完全退出（因为任务可能在读取Flash或更新LVGL）
        vTaskDelay(pdMS_TO_TICKS(800));
        
        // 设置背景为白色（在删除子对象后设置，确保背景显示）
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        // 强制刷新屏幕背景
        lv_obj_invalidate(screen);
        lv_refr_now(nullptr);
        
        // 显示二维码图片
        if (qrcode_img_) {
            lv_obj_t* img = lv_img_create(screen);
            if (img == nullptr) {
                ESP_LOGE(TAG, "EnterWifiConfig: Failed to create image object");
                return;
            }
            lv_img_set_src(img, qrcode_img_);
            // 设置二维码颜色为黑色（在白色背景上更清晰）
            lv_obj_set_style_img_recolor(img, lv_color_black(), 0);
            lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
            // 确保图片可见
            lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_img_opa(img, LV_OPA_COVER, 0);
            lv_obj_center(img);
            // 确保二维码图片在最前面
            lv_obj_move_foreground(img);
            // 强制刷新图片对象
            lv_obj_invalidate(img);
            // 强制刷新，确保二维码立即显示
            lv_refr_now(nullptr);
            ESP_LOGI(TAG, "EnterWifiConfig: QR code image created successfully, img=%p", img);
        }
    } else {
        ESP_LOGW(TAG, "EnterWifiConfig: qrcode_img_ is null");
    }
}

void EyeDisplay::EnterOTAMode() {
    ESP_LOGI(TAG, "EnterOTAMode");
    
    // 禁用表情切换
    emotion_disabled_ = true;
    
    DisplayLockGuard lock(this);
    
    // 清空屏幕
    auto screen = lv_screen_active();
    lv_obj_clean(screen);
    
    // 设置黑色背景
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    
    // 创建圆环
    ota_progress_bar_ = lv_arc_create(screen);
    lv_obj_set_size(ota_progress_bar_, height_ - 4, height_ - 4);  // 设置大小为屏幕高度的3/8
    lv_obj_align(ota_progress_bar_, LV_ALIGN_CENTER, 0, 0);  // 居中显示
    lv_arc_set_value(ota_progress_bar_, 0);  // 设置当前值
    lv_arc_set_bg_angles(ota_progress_bar_, 0, 360);  // 设置背景弧角度
    lv_arc_set_rotation(ota_progress_bar_, 270);  // 设置旋转角度，从顶部开始
    lv_obj_remove_style(ota_progress_bar_, NULL, LV_PART_KNOB);  // 去除旋钮
    lv_obj_clear_flag(ota_progress_bar_, LV_OBJ_FLAG_CLICKABLE);  // 去除可点击属性
    
    // 设置背景弧宽度和颜色
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_black(), LV_PART_MAIN);
    
    // 设置前景弧宽度和颜色
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    
    // 创建百分比标签
    ota_number_label_ = lv_label_create(screen);
    lv_obj_align(ota_number_label_, LV_ALIGN_CENTER, 0, 0);  // 居中显示
    lv_label_set_text(ota_number_label_, "0%");  // 设置文本
    lv_obj_set_style_text_font(ota_number_label_, fonts_.text_font, LV_STATE_DEFAULT);  // 设置字体
    lv_obj_set_style_text_color(ota_number_label_, lv_color_hex(EYE_COLOR), 0);  // 设置文字颜色
    
    // 重置进度
    ota_progress_ = 0;
    
    ESP_LOGI(TAG, "OTA mode initialized");
}

void EyeDisplay::SetOTAProgress(int progress) {
    if (ota_progress_bar_ == nullptr || ota_number_label_ == nullptr) {
        ESP_LOGW(TAG, "OTA mode not initialized");
        return;
    }
    
    // 限制进度范围
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    
    ota_progress_ = progress;
    
    DisplayLockGuard lock(this);
    
    // 更新进度条
    lv_arc_set_value(ota_progress_bar_, progress);
    
    // 更新百分比标签
    char progress_str[8];
    snprintf(progress_str, sizeof(progress_str), "%d%%", progress);
    lv_label_set_text(ota_number_label_, progress_str);
    
    ESP_LOGI(TAG, "OTA Progress: %d%%", progress);
} 

void EyeDisplay::EnterTestMode() {
    ESP_LOGI(TAG, "EnterTestMode");
    
    DisplayLockGuard lock(this);
    
    // 设置测试模式标志
    test_mode_active_ = true;
    
    // 停止所有当前动画
    lv_anim_del(left_eye_, nullptr);
    lv_anim_del(right_eye_, nullptr);
    lv_anim_del(mouth_, nullptr);
    lv_anim_del(left_hand_, nullptr);
    lv_anim_del(right_hand_, nullptr);
    
    // 清空屏幕
    auto screen = lv_screen_active();
    lv_obj_clean(screen);
    
    // 设置黑色背景
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    
    // 创建产测模式容器
    test_mode_container_ = lv_obj_create(screen);
    lv_obj_set_size(test_mode_container_, width_, height_);
    lv_obj_set_style_bg_color(test_mode_container_, lv_color_black(), 0);
    lv_obj_set_style_border_width(test_mode_container_, 0, 0);
    lv_obj_set_style_pad_all(test_mode_container_, 10, 0);
    lv_obj_align(test_mode_container_, LV_ALIGN_CENTER, 0, 0);
    
    // 创建标题
    test_mode_title_ = lv_label_create(test_mode_container_);
    lv_label_set_text(test_mode_title_, "Production Test");
    lv_obj_set_style_text_font(test_mode_title_, fonts_.text_font, 0);
    lv_obj_set_style_text_color(test_mode_title_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_align(test_mode_title_, LV_ALIGN_TOP_MID, 0, 5);  // 调整位置
    
    // 创建测试项列表容器
    test_mode_list_ = lv_obj_create(test_mode_container_);
    lv_obj_set_size(test_mode_list_, width_ - 20, height_ - 50);  // 增加高度
    lv_obj_set_style_bg_color(test_mode_list_, lv_color_black(), 0);
    lv_obj_set_style_border_width(test_mode_list_, 1, 0);
    lv_obj_set_style_border_color(test_mode_list_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_pad_all(test_mode_list_, 3, 0);  // 减小内边距
    lv_obj_align(test_mode_list_, LV_ALIGN_TOP_MID, 0, 35);  // 调整位置
    
    // 清空测试项标签映射
    test_item_labels_.clear();
    
    ESP_LOGI(TAG, "Test mode initialized");
}

void EyeDisplay::StartRGBTest() {
    ESP_LOGI(TAG, "StartRGBTest");
    DisplayLockGuard lock(this);

    // 标记
    rgb_test_active_ = true;
    test_mode_active_ = true; // 在测试期间也禁止表情切换

    // 清空屏幕
    auto screen = lv_screen_active();
    lv_obj_clean(screen);

    // 初始化阶段
    rgb_test_phase_ = 0;

    // 立即设置第一种颜色（红）
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFF0000), 0);

    // 创建或重启定时器，每3秒切换颜色
    if (rgb_test_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                if (!self->rgb_test_active_) return;
                DisplayLockGuard lock(self);
                auto screen = lv_screen_active();
                self->rgb_test_phase_ = (self->rgb_test_phase_ + 1) % 3;
                switch (self->rgb_test_phase_) {
                    case 0: // 红
                        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFF0000), 0);
                        break;
                    case 1: // 蓝
                        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000FF), 0);
                        break;
                    case 2: // 白
                        lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), 0);
                        break;
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "rgb_test_timer"
        };
        esp_timer_create(&args, &rgb_test_timer_);
    }
    esp_timer_stop(rgb_test_timer_);
    esp_timer_start_periodic(rgb_test_timer_, 3000000); // 3s
}

void EyeDisplay::StopRGBTest() {
    ESP_LOGI(TAG, "StopRGBTest");
    rgb_test_active_ = false;
    if (rgb_test_timer_) {
        esp_timer_stop(rgb_test_timer_);
    }

    // 结束后回到测试列表界面，并显示现有测试项
    EnterTestMode();
    SetTestItems(test_items_);
}

void EyeDisplay::SetTestItems(const std::vector<TestItem>& test_items) {
    if (test_mode_list_ == nullptr) {
        ESP_LOGW(TAG, "Test mode not initialized");
        return;
    }
    
    DisplayLockGuard lock(this);
    
    // 清空现有测试项标签
    for (auto& pair : test_item_labels_) {
        if (pair.second != nullptr) {
            lv_obj_del(pair.second);
        }
    }
    test_item_labels_.clear();
    
    // 保存测试项
    test_items_ = test_items;
    
    // 创建测试项标签
    int y_offset = 2;
    for (size_t i = 0; i < test_items_.size(); ++i) {
        const auto& item = test_items_[i];
        
        // 创建测试项容器
        lv_obj_t* item_container = lv_obj_create(test_mode_list_);
        lv_obj_set_size(item_container, width_ - 30, 18);  // 减小高度
        lv_obj_set_style_bg_color(item_container, lv_color_black(), 0);
        lv_obj_set_style_border_width(item_container, 0, 0);
        lv_obj_set_style_pad_all(item_container, 1, 0);  // 减小内边距
        lv_obj_set_pos(item_container, 5, y_offset);
        
        // 创建测试项名称标签
        lv_obj_t* name_label = lv_label_create(item_container);
        lv_label_set_text(name_label, item.name.c_str());
        lv_obj_set_style_text_font(name_label, fonts_.text_font, 0);
        lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
        lv_obj_set_style_text_letter_space(name_label, 0, 0);  // 减小字间距
        lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 3, 0);
        
        // 创建测试状态标签
        lv_obj_t* status_label = lv_label_create(item_container);
        const char* text = (item.status == 1) ? "PASS" : ((item.status == 2) ? "FAIL" : "等待测试");
        lv_label_set_text(status_label, text);
        lv_obj_set_style_text_font(status_label, fonts_.text_font, 0);
        lv_color_t color = (item.status == 1) ? lv_color_hex(0x00FF00) : ((item.status == 2) ? lv_color_hex(0xFF0000) : lv_color_hex(0xFFFF00));
        lv_obj_set_style_text_color(status_label, color, 0);
        lv_obj_set_style_text_letter_space(status_label, 0, 0);  // 减小字间距
        lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -3, 0);
        
        // 保存状态标签的引用
        test_item_labels_[item.id] = status_label;
        
        y_offset += 20;  // 减小行间距
    }
    
    ESP_LOGI(TAG, "Set %zu test items", test_items_.size());
}

void EyeDisplay::UpdateTestItem(const std::string& id, bool pass) {
    if (rgb_test_active_) {
        ESP_LOGW(TAG, "RGB test active, ignore update test item");
        return;
    }

    if (test_mode_list_ == nullptr) {
        ESP_LOGW(TAG, "Test mode not initialized");
        return;
    }
    
    // 查找对应的测试项
    auto it = std::find_if(test_items_.begin(), test_items_.end(),
        [&id](const TestItem& item) { return item.id == id; });
    
    if (it == test_items_.end()) {
        ESP_LOGW(TAG, "Test item with id '%s' not found", id.c_str());
        return;
    }
    
    // 更新测试项状态（true->1, false->0 等待）
    it->status = pass ? 1 : 0;
    
    // 更新UI
    auto label_it = test_item_labels_.find(id);
    if (label_it != test_item_labels_.end()) {
        DisplayLockGuard lock(this);
        
        lv_label_set_text(label_it->second, pass ? "PASS" : "等待测试");
        lv_obj_set_style_text_color(label_it->second, pass ? lv_color_hex(0x00FF00) : lv_color_hex(0xFFFF00), 0);
    }
    
    ESP_LOGI(TAG, "Updated test item '%s': %s", id.c_str(), pass ? "PASS" : "FAIL");
}

void EyeDisplay::UpdateTestItemStatus(const std::string& id, int status) {
    if (rgb_test_active_) {
        ESP_LOGW(TAG, "RGB test active, ignore update test item status");
        return;
    }
    if (test_mode_list_ == nullptr) {
        ESP_LOGW(TAG, "Test mode not initialized");
        return;
    }
    auto it = std::find_if(test_items_.begin(), test_items_.end(),
        [&id](const TestItem& item) { return item.id == id; });
    if (it == test_items_.end()) {
        ESP_LOGW(TAG, "Test item with id '%s' not found", id.c_str());
        return;
    }
    if (status < 0) status = 0;
    if (status > 2) status = 2;
    it->status = status;
    auto label_it = test_item_labels_.find(id);
    if (label_it != test_item_labels_.end()) {
        DisplayLockGuard lock(this);
        const char* text = (status == 1) ? "PASS" : ((status == 2) ? "FAIL" : "等待测试");
        lv_label_set_text(label_it->second, text);
        lv_color_t color = (status == 1) ? lv_color_hex(0x00FF00) : ((status == 2) ? lv_color_hex(0xFF0000) : lv_color_hex(0xFFFF00));
        lv_obj_set_style_text_color(label_it->second, color, 0);
    }
    ESP_LOGI(TAG, "Updated test item '%s' status: %d", id.c_str(), status);
}

void EyeDisplay::ShowBatteryIndicator() {
    ESP_LOGI(TAG, "ShowBatteryIndicator: 显示电量圆环");
    
    // 保存当前表情状态，以便5秒后恢复
    const char* current_emotion = nullptr;
    switch (current_state_) {
        case EyeState::IDLE: current_emotion = "neutral"; break;
        case EyeState::HAPPY: current_emotion = "happy"; break;
        case EyeState::LAUGHING: current_emotion = "laughing"; break;
        case EyeState::SAD: current_emotion = "sad"; break;
        case EyeState::ANGRY: current_emotion = "angry"; break;
        case EyeState::CRYING: current_emotion = "crying"; break;
        case EyeState::LOVING: current_emotion = "loving"; break;
        case EyeState::EMBARRASSED: current_emotion = "embarrassed"; break;
        case EyeState::SURPRISED: current_emotion = "surprised"; break;
        case EyeState::SHOCKED: current_emotion = "shocked"; break;
        case EyeState::THINKING: current_emotion = "thinking"; break;
        case EyeState::WINKING: current_emotion = "winking"; break;
        case EyeState::COOL: current_emotion = "cool"; break;
        case EyeState::RELAXED: current_emotion = "relaxed"; break;
        case EyeState::DELICIOUS: current_emotion = "delicious"; break;
        case EyeState::KISSY: current_emotion = "kissy"; break;
        case EyeState::CONFIDENT: current_emotion = "confident"; break;
        case EyeState::SLEEPING: current_emotion = "sleepy"; break;
        case EyeState::SILLY: current_emotion = "silly"; break;
        case EyeState::VERTIGO: current_emotion = "vertigo"; break;
        case EyeState::CONFUSED: current_emotion = "confused"; break;
        default: current_emotion = "neutral"; break;  // 默认值
    }
    // 确保总是保存一个表情（即使current_emotion为nullptr，也使用neutral）
    if (current_emotion != nullptr) {
        saved_emotion_before_battery_ = current_emotion;
        ESP_LOGI(TAG, "保存当前表情: %s (状态: %d)", saved_emotion_before_battery_.c_str(), (int)current_state_);
    } else {
        saved_emotion_before_battery_ = "neutral";
        ESP_LOGW(TAG, "当前状态未映射到表情，使用默认neutral (状态: %d)", (int)current_state_);
    }
    
    // 获取电量信息
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
    
    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGW(TAG, "Screen is nullptr");
        return;
    }
    
    // 保存当前背景色并设置黑色背景
    saved_screen_bg_color_ = lv_obj_get_style_bg_color(screen, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    ESP_LOGI(TAG, "ShowBatteryIndicator: 设置黑色背景");
    
    // 隐藏视频图像对象（如果存在）
    // 视频图像对象是通过 lv_image_create 创建的，大小通常是 240x240
    uint32_t child_cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, i);
        if (child != nullptr && lv_obj_check_type(child, &lv_image_class)) {
            // 检查对象大小，视频图像通常是全屏大小（240x240）
            int32_t obj_w = lv_obj_get_width(child);
            int32_t obj_h = lv_obj_get_height(child);
            // 如果是全屏大小的图像对象，很可能是视频图像，隐藏它
            if (obj_w >= width_ - 10 && obj_h >= height_ - 10) {
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI(TAG, "ShowBatteryIndicator: 隐藏视频图像对象 (size: %dx%d)", obj_w, obj_h);
            }
        }
    }
    
    // 清空所有表情UI元素：隐藏眼睛、嘴巴、爱心、眼泪、zzz标签、手部等
    if (left_eye_ != nullptr) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(left_eye_, nullptr);  // 停止眼睛动画
    }
    if (right_eye_ != nullptr) {
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
        lv_anim_del(right_eye_, nullptr);  // 停止眼睛动画
    }
    // 隐藏眼睛容器
    if (left_eye_ != nullptr) {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container != nullptr) {
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // 停止并隐藏嘴巴
    if (mouth_ != nullptr) {
        lv_anim_del(mouth_, nullptr);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 停止并隐藏爱心
    if (left_heart_ != nullptr) {
        lv_anim_del(left_heart_, nullptr);
        lv_obj_add_flag(left_heart_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_heart_ != nullptr) {
        lv_anim_del(right_heart_, nullptr);
        lv_obj_add_flag(right_heart_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 隐藏眼泪
    if (right_tear_ != nullptr) {
        lv_obj_add_flag(right_tear_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 隐藏zzz标签
    if (zzz1_ != nullptr) {
        lv_obj_add_flag(zzz1_, LV_OBJ_FLAG_HIDDEN);
    }
    if (zzz2_ != nullptr) {
        lv_obj_add_flag(zzz2_, LV_OBJ_FLAG_HIDDEN);
    }
    if (zzz3_ != nullptr) {
        lv_obj_add_flag(zzz3_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 停止并隐藏手部
    if (left_hand_ != nullptr) {
        lv_anim_del(left_hand_, nullptr);
        lv_obj_add_flag(left_hand_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_hand_ != nullptr) {
        lv_anim_del(right_hand_, nullptr);
        lv_obj_add_flag(right_hand_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 如果已经存在，先删除
    if (battery_arc_ != nullptr) {
        lv_obj_del(battery_arc_);
        battery_arc_ = nullptr;
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
        battery_label_ = nullptr;
    }
    if (signal_img_ != nullptr) {
        lv_obj_del(signal_img_);
        signal_img_ = nullptr;
    }
    
    // 创建电量圆环（显示在屏幕最外侧）
    battery_arc_ = lv_arc_create(screen);
    // 圆环大小：使用屏幕高度，让圆环更大
    int arc_size = height_;  // 使用屏幕高度
    lv_obj_set_size(battery_arc_, arc_size, arc_size);
    lv_obj_align(battery_arc_, LV_ALIGN_CENTER, 0, 0);  // 居中显示
    lv_arc_set_range(battery_arc_, 0, 100);  // 设置范围
    lv_arc_set_bg_angles(battery_arc_, 0, 360);  // 设置背景弧角度（完整圆）
    lv_arc_set_rotation(battery_arc_, 270);  // 设置旋转角度，从顶部开始
    // 根据电量设置value，显示对应角度的圆环（电量百分比对应360度）
    lv_arc_set_value(battery_arc_, battery_level);  // 设置为电量值，显示对应角度
    lv_obj_remove_style(battery_arc_, NULL, LV_PART_KNOB);  // 去除旋钮
    lv_obj_clear_flag(battery_arc_, LV_OBJ_FLAG_CLICKABLE);  // 去除可点击属性
    
    // 前景弧颜色根据电量变化：电量>25%显示绿色，<=25%显示红色
    uint32_t arc_color = (battery_level > 25) ? 0x00FF00 : 0xFF0000;  // 绿色或红色
    
    // 隐藏背景弧，只显示电量对应的那一段
    lv_obj_set_style_arc_width(battery_arc_, 0, LV_PART_MAIN);  // 背景弧宽度设为0，隐藏背景
    lv_obj_set_style_arc_opa(battery_arc_, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景弧完全透明
    
    // 设置前景弧（根据电量显示对应角度：>25%绿色，<=25%红色）
    lv_obj_set_style_arc_width(battery_arc_, 8, LV_PART_INDICATOR);  // 圆环宽度8像素
    lv_obj_set_style_arc_color(battery_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);  // 前景弧颜色
    lv_obj_invalidate(battery_arc_);  // 强制刷新样式
    ESP_LOGI(TAG, "ShowBatteryIndicator: 创建圆环，电量: %d%%, 角度: %d度, 颜色: 0x%06X (%s)", 
             battery_level, (battery_level * 360) / 100, arc_color, (battery_level > 25) ? "绿色" : "红色");
    
    // 将圆环移到最前面
    lv_obj_move_foreground(battery_arc_);
    
    // 在圆环中心创建信号图标（使用图片而不是字体）
    // 获取当前网络状态图标，根据信号强度选择对应的图片
    const char* signal_icon = Board::GetInstance().GetNetworkStateIcon();
    const lv_image_dsc_t* wifi_img = nullptr;
    
    // 根据信号强度选择对应的 WiFi 图标
    if (signal_icon != nullptr) {
        if (strcmp(signal_icon, FONT_AWESOME_WIFI) == 0) {
            // 信号强 (rssi >= -60) → wifi_level_3_img
            wifi_img = &wifi_level_3_img;
            ESP_LOGI(TAG, "选择 WiFi 图标: wifi_level_3_img (信号强)");
        } else if (strcmp(signal_icon, FONT_AWESOME_WIFI_FAIR) == 0) {
            // 信号中等 (rssi >= -70) → wifi_level_2_img
            wifi_img = &wifi_level_2_img;
            ESP_LOGI(TAG, "选择 WiFi 图标: wifi_level_2_img (信号中等)");
        } else if (strcmp(signal_icon, FONT_AWESOME_WIFI_WEAK) == 0) {
            // 信号弱 (rssi < -70) → wifi_level_1_img
            wifi_img = &wifi_level_1_img;
            ESP_LOGI(TAG, "选择 WiFi 图标: wifi_level_1_img (信号弱)");
        }
    }
    
    // 如果找到了对应的图片，使用图片；否则使用字体图标作为后备
    if (wifi_img != nullptr) {
        signal_img_ = lv_image_create(screen);
        if (signal_img_ == nullptr) {
            ESP_LOGE(TAG, "创建信号图片对象失败");
            // 后备方案：使用字体图标
            signal_img_ = lv_label_create(screen);
            lv_obj_set_style_text_font(signal_img_, &font_awesome_30_4, 0);
            lv_obj_set_style_text_color(signal_img_, lv_color_hex(EYE_COLOR), 0);
            lv_obj_align(signal_img_, LV_ALIGN_CENTER, 0, 0);
            if (signal_icon != nullptr) {
                lv_label_set_text(signal_img_, signal_icon);
            } else {
                lv_label_set_text(signal_img_, FONT_AWESOME_WIFI_OFF);
            }
        } else {
            ESP_LOGI(TAG, "创建信号图片对象成功，设置图片源: w=%d, h=%d, cf=%d, data_size=%u, data=%p", 
                     wifi_img->header.w, wifi_img->header.h, wifi_img->header.cf, 
                     wifi_img->data_size, wifi_img->data);
            
            // 检查图片数据是否有效
            if (wifi_img->data == nullptr || wifi_img->data_size == 0) {
                ESP_LOGE(TAG, "图片数据无效: data=%p, data_size=%u", wifi_img->data, wifi_img->data_size);
                // 删除图片对象，使用字体图标
                lv_obj_del(signal_img_);
                signal_img_ = lv_label_create(screen);
                lv_obj_set_style_text_font(signal_img_, &font_awesome_30_4, 0);
                lv_obj_set_style_text_color(signal_img_, lv_color_hex(EYE_COLOR), 0);
                lv_obj_align(signal_img_, LV_ALIGN_CENTER, 0, 0);
                if (signal_icon != nullptr) {
                    lv_label_set_text(signal_img_, signal_icon);
                } else {
                    lv_label_set_text(signal_img_, FONT_AWESOME_WIFI_OFF);
                }
            } else {
                lv_img_set_src(signal_img_, wifi_img);
                // 设置图片大小（根据图片实际大小，120x120）
                lv_obj_set_size(signal_img_, wifi_img->header.w, wifi_img->header.h);
                lv_obj_align(signal_img_, LV_ALIGN_CENTER, 0, 0);  // 居中显示在圆环中心
                // 确保图片可见
                lv_obj_clear_flag(signal_img_, LV_OBJ_FLAG_HIDDEN);
                // 将图标颜色改为主题色（EYE_COLOR），背景保持黑色（透明）
                lv_obj_set_style_img_recolor(signal_img_, lv_color_hex(EYE_COLOR), 0);  // 设置为主题色
                lv_obj_set_style_img_recolor_opa(signal_img_, LV_OPA_COVER, 0);  // 完全不透明
                // 强制刷新图片
                lv_obj_invalidate(signal_img_);
                ESP_LOGI(TAG, "信号图片已设置: size=%dx%d, format=%d, data_size=%u, 主题色: 0x%06X", 
                         wifi_img->header.w, wifi_img->header.h, wifi_img->header.cf, wifi_img->data_size, EYE_COLOR);
            }
        }
    } else {
        // 后备方案：使用字体图标
        ESP_LOGI(TAG, "未找到对应的 WiFi 图片，使用字体图标");
        signal_img_ = lv_label_create(screen);
        lv_obj_set_style_text_font(signal_img_, &font_awesome_30_4, 0);
        lv_obj_set_style_text_color(signal_img_, lv_color_hex(EYE_COLOR), 0);
        lv_obj_align(signal_img_, LV_ALIGN_CENTER, 0, 0);
        if (signal_icon != nullptr) {
            lv_label_set_text(signal_img_, signal_icon);
        } else {
            lv_label_set_text(signal_img_, FONT_AWESOME_WIFI_OFF);
        }
    }
    
    // 将信号图标移到最前面（在圆环之上）
    lv_obj_move_foreground(signal_img_);
    
    ESP_LOGI(TAG, "电量圆环已显示: %d%%, 充电: %d", battery_level, charging);
    
    // 创建定时器，5秒后自动隐藏并恢复表情
    if (battery_display_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                // 使用 Application::Schedule 将恢复操作调度到主应用线程执行
                // 避免在定时器任务中直接调用可能导致阻塞的操作
                Application::GetInstance().Schedule([]() {
                    Board::GetInstance().HideBatteryIndicator();
                }, "HideBatteryIndicator_Timer");
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_display_timer"
        };
        esp_timer_create(&timer_args, &battery_display_timer_);
    }
    esp_timer_stop(battery_display_timer_);
    esp_timer_start_once(battery_display_timer_, 5000000);  // 5秒后隐藏
}

void EyeDisplay::HideBatteryIndicator() {
    ESP_LOGI(TAG, "HideBatteryIndicator: 隐藏双击显示的电量信号UI");
    
    // 检查是否在配网模式或OTA模式，这些模式下不应该恢复表情
    bool in_special_mode = emotion_disabled_ || test_mode_active_ || rgb_test_active_;
    
    // 停止定时器
    if (battery_display_timer_ != nullptr) {
        esp_timer_stop(battery_display_timer_);
    }
    
    std::string emotion_to_restore;
    bool was_video_mode = false;
    {
        DisplayLockGuard lock(this);
        // 删除双击显示的所有元素
        if (battery_arc_ != nullptr) {
            lv_obj_del(battery_arc_);
            battery_arc_ = nullptr;
        }
        if (battery_label_ != nullptr) {
            lv_obj_del(battery_label_);
            battery_label_ = nullptr;
        }
        if (signal_img_ != nullptr) {
            lv_obj_del(signal_img_);
            signal_img_ = nullptr;
        }
        
        // 保存要恢复的状态（在锁内）
        was_video_mode = was_video_mode_before_battery_;
        if (!saved_emotion_before_battery_.empty()) {
            emotion_to_restore = saved_emotion_before_battery_;
            saved_emotion_before_battery_.clear();
        }
        // 恢复背景色
        lv_obj_t* screen = lv_screen_active();
        if (screen != nullptr) {
            lv_obj_set_style_bg_color(screen, saved_screen_bg_color_, 0);
            ESP_LOGI(TAG, "HideBatteryIndicator: 恢复背景色");
        }
        // 清除视频模式信息
        was_video_mode_before_battery_ = false;
        saved_video_group_index_ = -1;
    }  // 锁在这里自动释放
    
    // 所有表情都通过视频播放，由外部的 HideBatteryIndicator() 恢复视频播放
    // 这里不再调用 ProcessEmotionChange()，因为所有表情都统一使用视频播放
    if (was_video_mode) {
        ESP_LOGI(TAG, "之前在视频模式，不恢复表情，由外部恢复视频播放");
    } else if (in_special_mode) {
        ESP_LOGI(TAG, "在特殊模式（配网/OTA/测试），不恢复表情");
    } else {
        // 理论上不应该到达这里，因为所有表情都通过视频播放
        // 如果确实需要恢复，应该通过 SetEmotion() 来触发视频播放
        ESP_LOGW(TAG, "HideBatteryIndicator: 非视频模式且不在特殊模式，但所有表情应通过视频播放");
        if (!emotion_to_restore.empty()) {
            ESP_LOGI(TAG, "理论上不应该执行：恢复之前保存的表情: %s (应通过视频播放)", emotion_to_restore.c_str());
        } else {
            ESP_LOGW(TAG, "没有保存的表情可恢复，saved_emotion_before_battery_为空");
        }
    }
}

void EyeDisplay::ShowBatteryIndicatorForCharging(int battery_level_param) {
    ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 充电时显示电量圆环");
    
    // 检查是否在测试模式或配网模式，这些模式下不显示电量圆环
    if (emotion_disabled_ || test_mode_active_ || rgb_test_active_) {
        ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 在特殊模式，不显示电量圆环");
        return;
    }
    
    // 获取电量信息
    // 注意：此函数可能在定时器回调中被调用，不能直接调用 Board::GetInstance()
    // 因为 Board::GetInstance() 可能在初始化时阻塞，导致死锁
    // 方案：使用传入的电量值，或者使用上次缓存的电量值，或者使用默认值
    int battery_level = 0;
    bool charging = true;  // 默认假设正在充电（因为此函数只在充电时调用）
    bool discharging = false;
    
    // 优先使用传入的电量值，否则使用上次缓存的电量值，最后使用默认值
    if (battery_level_param >= 0 && battery_level_param <= 100) {
        battery_level = battery_level_param;
    } else if (last_charging_battery_level_ > 0) {
        battery_level = last_charging_battery_level_;
    } else {
        // 如果还没有缓存值，使用默认值
        battery_level = 50;  // 默认50%
    }
    
    // 如果已经显示且圆环存在，只需要更新电量值（只在电量或颜色变化时更新）
    if (charging_indicator_showing_ && charging_battery_arc_ != nullptr) {
        ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 圆环已存在，检查有效性");
        DisplayLockGuard lock(this);
        // 再次检查圆环是否真的存在且有效（可能被意外删除）
        if (charging_battery_arc_ != nullptr && lv_obj_is_valid(charging_battery_arc_)) {
            ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 圆环有效，检查是否需要更新");
            // 计算当前颜色
            uint32_t arc_color = (battery_level > 25) ? 0x00FF00 : 0xFF0000;
            
            // 只在电量或颜色变化时才更新，避免不必要的刷新
            bool need_update = (battery_level != last_charging_battery_level_) || 
                              (arc_color != last_charging_arc_color_);
            
            if (need_update) {
                ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 需要更新 (battery_level: %d->%d, color: 0x%06X->0x%06X)", 
                         last_charging_battery_level_, battery_level, last_charging_arc_color_, arc_color);
                // 根据电量设置value，显示对应角度的圆环（电量百分比对应360度）
                lv_arc_set_value(charging_battery_arc_, battery_level);  // 设置为电量值，显示对应角度
                // 隐藏背景弧，只显示电量对应的那一段
                lv_obj_set_style_arc_width(charging_battery_arc_, 0, LV_PART_MAIN);  // 背景弧宽度设为0，隐藏背景
                lv_obj_set_style_arc_opa(charging_battery_arc_, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景弧完全透明
                lv_obj_set_style_arc_color(charging_battery_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);  // 前景弧颜色
                lv_obj_invalidate(charging_battery_arc_);  // 只在变化时强制刷新样式
                
                // 更新记录的值
                last_charging_battery_level_ = battery_level;
                last_charging_arc_color_ = arc_color;
                
                ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 已存在，更新电量: %d%%, 角度: %d度, 颜色: 0x%06X (%s)", 
                         battery_level, (battery_level * 360) / 100, arc_color, (battery_level > 25) ? "绿色" : "红色");
            } else {
                ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 电量未变化，跳过更新 (battery_level=%d, color=0x%06X)", 
                         battery_level, arc_color);
            }
            return;
        } else {
            ESP_LOGW(TAG, "ShowBatteryIndicatorForCharging: 圆环标记为存在但对象无效，重新创建 (charging_battery_arc_=%p, valid=%d)", 
                     charging_battery_arc_, charging_battery_arc_ != nullptr ? lv_obj_is_valid(charging_battery_arc_) : 0);
            charging_battery_arc_ = nullptr;
            charging_indicator_showing_ = false;
            last_charging_battery_level_ = -1;
            last_charging_arc_color_ = 0;
        }
    } else {
        ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 圆环不存在，需要创建");
    }
    
    DisplayLockGuard lock(this);
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGE(TAG, "ShowBatteryIndicatorForCharging: Screen is nullptr，无法创建圆环");
        return;
    }
    ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: Screen有效，开始创建圆环 (screen=%p)", screen);
    
    // 如果已经存在，先删除
    if (charging_battery_arc_ != nullptr) {
        ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 删除旧的圆环对象");
        lv_obj_del(charging_battery_arc_);
        charging_battery_arc_ = nullptr;
    }
    
    // 创建充电时的电量圆环（显示在屏幕最外侧，独立对象）
    ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 创建新的圆环对象");
    charging_battery_arc_ = lv_arc_create(screen);
    if (charging_battery_arc_ == nullptr) {
        ESP_LOGE(TAG, "ShowBatteryIndicatorForCharging: 创建圆环失败，lv_arc_create返回nullptr");
        return;
    }
    ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 圆环对象创建成功 (charging_battery_arc_=%p)", charging_battery_arc_);
    // 圆环大小：使用屏幕高度，让圆环更大
    int arc_size = height_;  // 使用屏幕高度
    lv_obj_set_size(charging_battery_arc_, arc_size, arc_size);
    lv_obj_align(charging_battery_arc_, LV_ALIGN_CENTER, 0, 0);  // 居中显示
    lv_arc_set_range(charging_battery_arc_, 0, 100);  // 设置范围
    lv_arc_set_bg_angles(charging_battery_arc_, 0, 360);  // 设置背景弧角度（完整圆）
    lv_arc_set_rotation(charging_battery_arc_, 270);  // 设置旋转角度，从顶部开始
    // 根据电量设置value，显示对应角度的圆环（电量百分比对应360度）
    lv_arc_set_value(charging_battery_arc_, battery_level);  // 设置为电量值，显示对应角度
    lv_obj_remove_style(charging_battery_arc_, NULL, LV_PART_KNOB);  // 去除旋钮
    lv_obj_clear_flag(charging_battery_arc_, LV_OBJ_FLAG_CLICKABLE);  // 去除可点击属性
    
    // 前景弧颜色根据电量变化：电量>25%显示绿色，<=25%显示红色
    uint32_t arc_color = (battery_level > 25) ? 0x00FF00 : 0xFF0000;  // 绿色或红色
    
    // 隐藏背景弧，只显示电量对应的那一段
    lv_obj_set_style_arc_width(charging_battery_arc_, 0, LV_PART_MAIN);  // 背景弧宽度设为0，隐藏背景
    lv_obj_set_style_arc_opa(charging_battery_arc_, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景弧完全透明
    
    // 设置前景弧（根据电量显示对应角度：>25%绿色，<=25%红色）
    lv_obj_set_style_arc_width(charging_battery_arc_, 8, LV_PART_INDICATOR);  // 圆环宽度8像素
    lv_obj_set_style_arc_color(charging_battery_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);  // 前景弧颜色
    lv_obj_invalidate(charging_battery_arc_);  // 强制刷新样式
    ESP_LOGI(TAG, "ShowBatteryIndicatorForCharging: 创建圆环，电量: %d%%, 角度: %d度, 颜色: 0x%06X (%s)", 
             battery_level, (battery_level * 360) / 100, arc_color, (battery_level > 25) ? "绿色" : "红色");
    
    // 将圆环移到最前面（确保在所有内容之上，包括视频图像）
    lv_obj_move_foreground(charging_battery_arc_);
    
    // 确保圆环可见
    lv_obj_clear_flag(charging_battery_arc_, LV_OBJ_FLAG_HIDDEN);
    
    // 设置圆环的父对象为屏幕，确保它始终显示在最上层
    // 注意：不创建信号图标，只显示圆环
    
    ESP_LOGI(TAG, "充电时电量圆环已创建: %d%%, 充电: %d, charging_battery_arc_=%p", 
             battery_level, charging, charging_battery_arc_);
    
    // 标记正在显示充电时的电量圆环
    charging_indicator_showing_ = true;
    
    // 记录当前电量和颜色，用于后续比较
    last_charging_battery_level_ = battery_level;
    last_charging_arc_color_ = arc_color;
    
    // 创建定时器，定期更新电量（每5秒更新一次）
    if (battery_charging_update_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                EyeDisplay* display = static_cast<EyeDisplay*>(arg);
                // 如果还在充电，更新电量显示
                if (display->charging_indicator_showing_) {
                    // 尝试获取最新电量（定时器在ESP_TIMER_TASK模式下运行，可以安全调用）
                    int battery_level = 0;
                    bool charging = false;
                    bool discharging = false;
                    bool got_battery_level = false;
                    
                    // 尝试获取最新电量，如果失败则使用缓存值
                    // 注意：定时器在ESP_TIMER_TASK模式下运行，可以安全调用Board::GetInstance()
                    if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
                        got_battery_level = true;
                        // 如果不再充电，停止更新
                        if (!charging) {
                            ESP_LOGI(TAG, "定时器更新: 检测到不再充电，停止更新");
                            display->charging_indicator_showing_ = false;
                            return;
                        }
                    } else {
                        // 如果获取失败，使用缓存值
                        ESP_LOGW(TAG, "定时器更新: 获取电量失败，使用缓存值");
                    }
                    
                    // 如果获取失败，使用缓存值
                    if (!got_battery_level) {
                        battery_level = display->last_charging_battery_level_ > 0 ? 
                                       display->last_charging_battery_level_ : 50;
                    }
                    
                    if (display->charging_battery_arc_ != nullptr) {
                        DisplayLockGuard lock(display);
                        // 检查对象是否仍然有效（可能已被删除）
                        if (display->charging_battery_arc_ != nullptr && lv_obj_is_valid(display->charging_battery_arc_)) {
                            // 计算当前颜色
                            uint32_t arc_color = (battery_level > 25) ? 0x00FF00 : 0xFF0000;
                            
                            // 只在电量或颜色变化时才更新，避免不必要的刷新
                            bool need_update = (battery_level != display->last_charging_battery_level_) || 
                                              (arc_color != display->last_charging_arc_color_);
                            
                            if (need_update) {
                                // 根据电量设置value，显示对应角度的圆环（电量百分比对应360度）
                                lv_arc_set_value(display->charging_battery_arc_, battery_level);  // 设置为电量值，显示对应角度
                                // 隐藏背景弧，只显示电量对应的那一段
                                lv_obj_set_style_arc_width(display->charging_battery_arc_, 0, LV_PART_MAIN);  // 背景弧宽度设为0，隐藏背景
                                lv_obj_set_style_arc_opa(display->charging_battery_arc_, LV_OPA_TRANSP, LV_PART_MAIN);  // 背景弧完全透明
                                lv_obj_set_style_arc_color(display->charging_battery_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);  // 前景弧颜色
                                lv_obj_invalidate(display->charging_battery_arc_);  // 只在变化时强制刷新样式
                                
                                // 更新记录的值
                                display->last_charging_battery_level_ = battery_level;
                                display->last_charging_arc_color_ = arc_color;
                                
                                ESP_LOGI(TAG, "定时器更新电量: %d%%, 颜色: 0x%06X (%s)", 
                                         battery_level, arc_color, (battery_level > 25) ? "绿色" : "红色");
                            }
                            // 确保圆环在最前面（但不移动，避免刷新）
                            // 注意：在视频模式下，EnsureChargingBatteryArcOnTop会处理这个
                        } else {
                            ESP_LOGW(TAG, "定时器更新: 圆环对象已失效，重置状态");
                            display->charging_battery_arc_ = nullptr;
                            display->charging_indicator_showing_ = false;
                            display->last_charging_battery_level_ = -1;
                            display->last_charging_arc_color_ = 0;
                        }
                    }
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_charging_update_timer"
        };
        esp_timer_create(&timer_args, &battery_charging_update_timer_);
    }
    esp_timer_stop(battery_charging_update_timer_);
    esp_timer_start_periodic(battery_charging_update_timer_, 5000000);  // 每5秒更新一次
}

void EyeDisplay::HideBatteryIndicatorForCharging() {
    ESP_LOGI(TAG, "HideBatteryIndicatorForCharging: 隐藏充电时的电量圆环");
    
    // 停止充电更新定时器
    if (battery_charging_update_timer_ != nullptr) {
        esp_timer_stop(battery_charging_update_timer_);
    }
    charging_indicator_showing_ = false;
    
    DisplayLockGuard lock(this);
    if (charging_battery_arc_ != nullptr) {
        lv_obj_del(charging_battery_arc_);
        charging_battery_arc_ = nullptr;
    }
    
    // 重置记录的值
    last_charging_battery_level_ = -1;
    last_charging_arc_color_ = 0;
}

void EyeDisplay::EnsureChargingBatteryArcOnTop(bool already_locked) {
    // 此函数已不再需要频繁调用
    // 充电圆环在充电时创建后一直显示，由定时器更新电量
    // 保留此函数以防其他地方调用，但实现为空或最小化操作
    
    // 如果圆环存在且有效，只确保它在最前面（仅在必要时调用，不频繁刷新）
    if (charging_indicator_showing_ && charging_battery_arc_ != nullptr) {
        if (lv_obj_is_valid(charging_battery_arc_)) {
            // 只在已经持有锁时才操作，避免频繁刷新
            if (already_locked) {
                // 只检查是否隐藏，不频繁移动位置（避免刷新）
                if (lv_obj_has_flag(charging_battery_arc_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_clear_flag(charging_battery_arc_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            // 如果没有锁，不操作（避免阻塞视频播放）
        }
    }
}

