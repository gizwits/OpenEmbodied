#include "eye_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>

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

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 80;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
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
        return;
    }
    // VERTIGO锁定：如果正在锁定且不是VERTIGO请求，直接忽略
    if (vertigo_locked_ && strcmp(emotion, "vertigo") != 0) {
        ESP_LOGI(TAG, "VERTIGO locked, ignore emotion: %s", emotion);
        return;
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
        new_state = EyeState::SAD;
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
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "winking") == 0) {
        new_state = EyeState::HAPPY;
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

    if (new_state == current_state_) {
        return;
    }

    // 使用锁保护状态切换
    DisplayLockGuard lock(this);

    // 如果不是睡眠状态，删除 zzz 标签
    if (current_state_ == EyeState::SLEEPING && new_state != EyeState::SLEEPING) {
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

    // 确保眼睛对象可见并重置为默认状态
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 重置眼睛尺寸为默认值（除了睡眠状态）
    if (new_state != EyeState::SLEEPING) {
        lv_obj_set_size(left_eye_, 40, 80);
        lv_obj_set_size(right_eye_, 40, 80);
        lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    }

    current_state_ = new_state;

    // 停止当前动画
    lv_anim_del(left_eye_, nullptr);
    lv_anim_del(right_eye_, nullptr);

    // 根据状态启动新的动画
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

    // VERTIGO状态，启动锁定和定时器
    if (current_state_ == EyeState::VERTIGO) {
        vertigo_locked_ = true;
        vertigo_unlock_time_ = esp_timer_get_time() + 5000000LL; // 5秒后解锁
        if (vertigo_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                    self->vertigo_locked_ = false;
                    ESP_LOGI(TAG, "VERTIGO unlock, auto switch to idle");
                    self->SetEmotion("neutral");
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "vertigo_timer"
            };
            esp_timer_create(&timer_args, &vertigo_timer_);
        }
        esp_timer_stop(vertigo_timer_);
        esp_timer_start_once(vertigo_timer_, 5000000); // 5秒
    } else {
        // 非VERTIGO状态，确保锁定解除
        vertigo_locked_ = false;
        if (vertigo_timer_) esp_timer_stop(vertigo_timer_);
    }
}

void EyeDisplay::StartIdleAnimation() {
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

void EyeDisplay::StartHappyAnimation() {
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

    // 创建嘴巴图片对象
    mouth_ = lv_img_create(lv_scr_act());
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 52);  // 居中，距离底部52像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(0xFCCCE6), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 52, height_ - 62);  // 在-52到-62像素之间移动
    lv_anim_set_time(&mouth_anim_, 1500);
    lv_anim_set_delay(&mouth_anim_, 0);
    lv_anim_set_exec_cb(&mouth_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, 1500);
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
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 52);  // 居中，距离底部52像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(0xFCCCE6), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度
    
    // 旋转嘴巴180度，使其变成向上的箭头
    lv_obj_set_style_transform_angle(mouth_, 1800, 0);  // 180度 = 1800 * 0.1度
    
    // 重新调整位置，确保旋转后仍然居中
    lv_obj_set_pos(mouth_, (width_ + 32) / 2, height_ - 32);

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 32, height_ - 42); 
    lv_anim_set_time(&mouth_anim_, 1500);
    lv_anim_set_delay(&mouth_anim_, 0);
    lv_anim_set_exec_cb(&mouth_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, 1500);
    lv_anim_set_playback_delay(&mouth_anim_, 0);
    lv_anim_start(&mouth_anim_);

    // 创建右眼眼泪（椭圆）
    right_tear_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(right_tear_, 12, 20); // 椭圆形状
    lv_obj_set_style_radius(right_tear_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear_, lv_color_hex(0xFCCCE6), 0); // 青色
    lv_obj_set_style_border_width(right_tear_, 0, 0);
    lv_obj_set_style_border_side(right_tear_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_tear_, 0, 0);
    lv_obj_set_style_shadow_width(right_tear_, 0, 0);
    lv_obj_set_style_outline_width(right_tear_, 0, 0);

    // 位置在屏幕右侧，眼睛下方
    lv_obj_set_pos(right_tear_, width_ - 60, height_ / 2 + 20);
    // 添加下落动画
    static lv_anim_t tear_anim;
    lv_anim_init(&tear_anim);
    lv_anim_set_var(&tear_anim, right_tear_);
    lv_anim_set_values(&tear_anim, height_ / 2 + 40, height_ / 2 + 20);
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
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(0xFCCCE6), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, 0, 0);  // 左眼位置，距离左边缘40像素
    
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &spiral_img_64);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(0xFCCCE6), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, -0, 0);  // 右眼位置，距离右边缘40像素
    
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
    lv_anim_set_time(&left_anim_, 1500);  // 从3000ms减少到1500ms，转得更快
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&left_anim_);
    
    // 为右眼爱心添加循环旋转动画
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_heart);
    lv_anim_set_values(&right_anim_, 0, 3600);  // 从0度旋转到-3600度（逆时针10圈）
    lv_anim_set_time(&right_anim_, 1500);  // 从3000ms减少到1500ms，转得更快
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&right_anim_);
}


void EyeDisplay::StartLovingAnimation() {
    // 隐藏原来的圆形眼睛
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 创建左眼爱心图片
    lv_obj_t* left_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(left_heart, &hart_img_64);
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(0xFCCCE6), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, 0, 0);  // 左眼位置，距离左边缘40像素
    
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &hart_img_64);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(0xFCCCE6), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, -0, 0);  // 右眼位置，距离右边缘40像素
    
    // 保存爱心对象指针，以便在状态切换时清理
    left_heart_ = left_heart;
    right_heart_ = right_heart;

    // 左眼爱心放大缩小动画
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_heart);
    lv_anim_set_values(&left_anim_, 64, 128);  // 从100%放大到200%
    lv_anim_set_time(&left_anim_, 750);         // 放大时间
    lv_anim_set_playback_time(&left_anim_, 750);// 缩小时间
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_start(&left_anim_);

    // 右眼爱心放大缩小动画
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_heart);
    lv_anim_set_values(&right_anim_, 64, 128);  // 从100%放大到200%
    lv_anim_set_time(&right_anim_, 750);
    lv_anim_set_playback_time(&right_anim_, 750);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
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

    // 创建三个 z 标签
    zzz1_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz1_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz1_, lv_color_hex(0xFCCCE6), 0);  // 黄色
    lv_label_set_text(zzz1_, "z");
    lv_obj_align(zzz1_, LV_ALIGN_TOP_MID, -40, 50);  // 调整垂直位置到 50
    lv_obj_set_style_text_letter_space(zzz1_, 2, 0);  // 增加字间距

    zzz2_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz2_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz2_, lv_color_hex(0xFCCCE6), 0);  // 黄色
    lv_label_set_text(zzz2_, "z");
    lv_obj_align(zzz2_, LV_ALIGN_TOP_MID, 0, 40);  // 调整垂直位置到 40
    lv_obj_set_style_text_letter_space(zzz2_, 2, 0);  // 增加字间距

    zzz3_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz3_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz3_, lv_color_hex(0xFCCCE6), 0);  // 黄色
    lv_label_set_text(zzz3_, "z");
    lv_obj_align(zzz3_, LV_ALIGN_TOP_MID, 40, 30);  // 调整垂直位置到 30
    lv_obj_set_style_text_letter_space(zzz3_, 2, 0);  // 增加字间距
}

void EyeDisplay::StartShockedAnimation() {
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
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Create left eye
    left_eye_ = lv_obj_create(container);
    lv_obj_set_size(left_eye_, 40, 80);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(0xFCCCE6), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_eye_, 0, 0);
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);
    lv_obj_set_style_outline_width(left_eye_, 0, 0);

    // Create right eye
    right_eye_ = lv_obj_create(container);
    lv_obj_set_size(right_eye_, 40, 80);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(0xFCCCE6), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_eye_, 0, 0);
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);
    lv_obj_set_style_outline_width(right_eye_, 0, 0);

    // 启动默认的待机动画
    StartIdleAnimation();
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

void EyeDisplay::EnterWifiConifg() {
    ESP_LOGI(TAG, "EnterWifiConifg");
    if (qrcode_img_) {
        ESP_LOGI(TAG, "EnterWifiConifg qrcode_img_ is not null");
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        // 设置背景为白色
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        // 删除所有子对象（清空屏幕）
        lv_obj_clean(screen);
        // 显示二维码图片
        if (qrcode_img_) {
            lv_obj_t* img = lv_img_create(screen);
            lv_img_set_src(img, qrcode_img_);
            lv_obj_set_style_img_recolor(img, lv_color_hex(0xFCCCE6), 0);
            lv_obj_center(img);
        }
    }
} 