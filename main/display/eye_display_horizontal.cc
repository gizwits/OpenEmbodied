#include "eye_display_horizontal.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>
#include "application.h"

#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

#define TAG "EyeDisplayHorizontal"

EyeDisplayHorizontal::EyeDisplayHorizontal(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    const lv_img_dsc_t* qrcode_img,
    DisplayFonts fonts)
: EyeDisplayHorizontal(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, fonts) // 委托构造
{
    qrcode_img_ = qrcode_img;
}


EyeDisplayHorizontal::EyeDisplayHorizontal(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy,
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
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
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

EyeDisplayHorizontal::~EyeDisplayHorizontal() {
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

bool EyeDisplayHorizontal::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EyeDisplayHorizontal::Unlock() {
    lvgl_port_unlock();
}

void EyeDisplayHorizontal::SetEmotion(const char* emotion) {
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

void EyeDisplayHorizontal::EmotionTask(void* arg) {
    EyeDisplayHorizontal* display = static_cast<EyeDisplayHorizontal*>(arg);
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

void EyeDisplayHorizontal::ProcessEmotionChange(const char* emotion) {
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

    // 确保眼睛对象可见并重置为默认状态
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 重置眼睛旋转角度（特别是从生气状态切换出来时）
    lv_obj_set_style_transform_angle(left_eye_, 0, 0);
    lv_obj_set_style_transform_angle(right_eye_, 0, 0);
    // 恢复默认左右顺序（行方向），避免上一个状态改为反向后影响其它状态
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
        }
    }
    
    // 重置眼睛尺寸为默认值（除了睡眠状态） - 适配横屏
    if (new_state != EyeState::SLEEPING) {
        lv_obj_set_size(left_eye_, 30, 60);  // 适配横屏尺寸
        lv_obj_set_size(right_eye_, 30, 60);  // 适配横屏尺寸
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
                    EyeDisplayHorizontal* self = static_cast<EyeDisplayHorizontal*>(arg);
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

void EyeDisplayHorizontal::StartIdleAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 60);  // 适配横屏尺寸
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
    lv_anim_set_values(&right_anim_, 30, 60);  // 适配横屏尺寸
    lv_anim_set_time(&right_anim_, 600);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 600);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplayHorizontal::StartHappyAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 调整happy表情的眼睛位置，往上调25像素（5+20）
    lv_obj_t* container = lv_obj_get_parent(left_eye_);
    lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 20, 0);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 60);  // 适配横屏尺寸
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
    lv_anim_set_values(&right_anim_, 30, 60);  // 适配横屏尺寸
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
    lv_obj_set_pos(mouth_, (width_ - 30) / 2, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 15);  // 适配横屏，happy表情嘴巴往下调15像素（10+5）
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 15, height_ - 50 - DISPLAY_VERTICAL_OFFSET + 15);  // 适配横屏，happy表情嘴巴往下调15像素（10+5）
    lv_anim_set_time(&mouth_anim_, 1000);
    lv_anim_set_delay(&mouth_anim_, 0);
    lv_anim_set_exec_cb(&mouth_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, 1000);
    lv_anim_set_playback_delay(&mouth_anim_, 0);
    lv_anim_start(&mouth_anim_);
}

void EyeDisplayHorizontal::StartSadAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    // 将眼睛整体上移10像素
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 10, 0);
        }
    }
    // 保持左右眼原始顺序；哭泣仅显示右眼的眼泪
    
    // 设置眼睛为水平长条（减小厚度/高度）
    lv_obj_set_size(left_eye_, 45, 10);  // 厚度由15降到12
    lv_obj_set_size(right_eye_, 45, 10);  // 厚度由15降到12
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    

    // 创建嘴巴图片对象
    mouth_ = lv_img_create(lv_scr_act());
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 24) / 2, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 50);  // sad表情：整体下移50像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);  // 设置青色
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);  // 设置不透明度
    
    // 旋转嘴巴180度，使其变成向上的箭头
    lv_obj_set_style_transform_angle(mouth_, 1800, 0);  // 180度 = 1800 * 0.1度
    
    // 重新调整位置，确保旋转后仍然居中
    lv_obj_set_pos(mouth_, (width_ + 32) / 2, height_ - 32 - DISPLAY_VERTICAL_OFFSET + 30);

    // 创建嘴巴动画
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, mouth_);
    lv_anim_set_values(&mouth_anim_, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 50, height_ - 50 - DISPLAY_VERTICAL_OFFSET + 50);  // sad表情：动画基线下移50
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
    lv_obj_set_size(right_tear_, 6, 12); // 减小高度（厚度），长度保持
    lv_obj_set_style_radius(right_tear_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear_, lv_color_hex(EYE_COLOR), 0); // 青色
    lv_obj_set_style_border_width(right_tear_, 0, 0);
    lv_obj_set_style_border_side(right_tear_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_tear_, 0, 0);
    lv_obj_set_style_shadow_width(right_tear_, 0, 0);
    lv_obj_set_style_outline_width(right_tear_, 0, 0);

    // 将眼泪对齐到右眼下方（相对于右眼定位，避免左右颠倒）
    lv_obj_align_to(right_tear_, left_eye_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    // 记录基线Y，用于动画
    int base_y = lv_obj_get_y(right_tear_);
    // 添加下落动画（围绕当前对齐位置上下浮动）
    static lv_anim_t tear_anim;
    lv_anim_init(&tear_anim);
    lv_anim_set_var(&tear_anim, right_tear_);
    lv_anim_set_values(&tear_anim, base_y + 58, base_y + 78);  // 自下向上
    lv_anim_set_time(&tear_anim, 1000);
    lv_anim_set_repeat_count(&tear_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&tear_anim, 1000);
    lv_anim_set_exec_cb(&tear_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&tear_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&tear_anim);
}


void EyeDisplayHorizontal::StartVertigoAnimation() {
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 创建左眼爱心图片
    lv_obj_t* left_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(left_heart, &spiral_img_64);
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, -20, -DISPLAY_VERTICAL_OFFSET);  // 左眼位置，适配横屏
    
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &spiral_img_64);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, 20, -DISPLAY_VERTICAL_OFFSET);  // 右眼位置，适配横屏
    
    // 缩小图片 - 适配横屏
    lv_img_set_zoom(left_heart, 86);  // 适配横屏尺寸
    lv_img_set_zoom(right_heart, 86);  // 适配横屏尺寸

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


void EyeDisplayHorizontal::StartLovingAnimation() {
    // 清空整个屏幕，删除所有子对象
    auto screen = lv_screen_active();
    lv_obj_clean(screen);
    // 禁用屏幕滚动与滚动条
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    
    // 重新设置屏幕背景色
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    
    // 重新创建容器
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET, 0);
    // 禁用容器滚动与滚动条，防止子项溢出触发滚动
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    
    // 重新创建眼睛对象（隐藏状态）
    left_eye_ = lv_obj_create(container);
    lv_obj_set_size(left_eye_, 30, 60);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_eye_, 0, 0);
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);
    lv_obj_set_style_outline_width(left_eye_, 0, 0);
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    
    right_eye_ = lv_obj_create(container);
    lv_obj_set_size(right_eye_, 30, 60);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_eye_, 0, 0);
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);
    lv_obj_set_style_outline_width(right_eye_, 0, 0);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 清理所有可能存在的对象指针
    mouth_ = nullptr;
    right_tear_ = nullptr;
    left_hand_ = nullptr;
    right_hand_ = nullptr;
    left_heart_ = nullptr;
    right_heart_ = nullptr;
    
    // 创建左眼爱心图片
    lv_obj_t* left_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(left_heart, &hart_img);
    lv_obj_set_style_img_recolor(left_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(left_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(left_heart, LV_ALIGN_LEFT_MID, -20, -DISPLAY_VERTICAL_OFFSET);  // 左眼位置，向左偏移18像素增加间距
 
    // 创建右眼爱心图片
    lv_obj_t* right_heart = lv_img_create(lv_screen_active());
    lv_img_set_src(right_heart, &hart_img);
    lv_obj_set_style_img_recolor(right_heart, lv_color_hex(EYE_COLOR), 0);  // 设置为白色
    lv_obj_set_style_img_recolor_opa(right_heart, LV_OPA_COVER, 0);  // 完全不透明
    lv_obj_align(right_heart, LV_ALIGN_RIGHT_MID, 20, -DISPLAY_VERTICAL_OFFSET);  // 右眼位置，向右偏移18像素增加间距
 
    // 保存爱心对象指针，以便在状态切换时清理
    left_heart_ = left_heart;
    right_heart_ = right_heart;

    // 左眼爱心放大缩小动画
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_heart);
    lv_anim_set_values(&left_anim_, 65, 95);  
    lv_anim_set_time(&left_anim_, 500);         // 放大时间
    lv_anim_set_playback_time(&left_anim_, 500);// 缩小时间
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_overshoot);
    lv_anim_start(&left_anim_);

    // 右眼爱心放大缩小动画
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_heart);
    lv_anim_set_values(&right_anim_, 65, 95);  
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_overshoot);
    lv_anim_start(&right_anim_);
}

void EyeDisplayHorizontal::StartSleepingAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 设置眼睛为水平长条
    lv_obj_set_size(left_eye_, 45, 15);  // 适配横屏尺寸
    lv_obj_set_size(right_eye_, 45, 15);  // 适配横屏尺寸
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);

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

void EyeDisplayHorizontal::StartShockedAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 60);  // 适配横屏尺寸
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
    lv_anim_set_values(&right_anim_, 30, 60);  // 适配横屏尺寸
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

void EyeDisplayHorizontal::StartSillyAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 60);  // 适配横屏尺寸
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
    lv_anim_set_values(&right_anim_, 30, 60);  // 适配横屏尺寸
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplayHorizontal::StartAngryAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 调整angry表情的眼睛位置，往上移动10像素
    lv_obj_t* container = lv_obj_get_parent(left_eye_);
    lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 15, 0); // 上移5像素
    
    // 设置眼睛为倾斜的形状（内高外低），适配160x128屏幕
    // 左眼：右高左低
    lv_obj_set_size(left_eye_, 30, 40);  // 缩小到适合屏幕的尺寸
    lv_obj_set_style_radius(left_eye_, 8, 0);  // 调整圆角
    lv_obj_set_style_transform_angle(left_eye_, -150, 0);  // -15度倾斜
    
    // 右眼：左高右低  
    lv_obj_set_size(right_eye_, 30, 40);  // 缩小到适合屏幕的尺寸
    lv_obj_set_style_radius(right_eye_, 8, 0);  // 调整圆角
    lv_obj_set_style_transform_angle(right_eye_, 150, 0);  // 15度倾斜
    
    // 眼睛微微跳动的动画（保持和以前一样的弧度效果）
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 35, 45);  // 保持较大的高度变化范围，创造明显的弧度效果
    lv_anim_set_time(&left_anim_, 500);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 500);
    lv_anim_start(&left_anim_);
    
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 35, 45);  // 保持较大的高度变化范围，创造明显的弧度效果
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_start(&right_anim_);
    
    // 创建生气的嘴巴（紧闭的横线），适配屏幕尺寸
    mouth_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(mouth_, 5, 6);  // 将嘴巴宽度缩小一半：从10改为5
    lv_obj_set_style_radius(mouth_, 1, 0);  // 调整圆角
    lv_obj_set_style_bg_color(mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_pad_all(mouth_, 0, 0);
    lv_obj_set_style_shadow_width(mouth_, 0, 0);
    lv_obj_set_style_outline_width(mouth_, 0, 0);
    lv_obj_set_pos(mouth_, (width_ - 5) / 2, height_ / 2 - DISPLAY_VERTICAL_OFFSET + 55);  // 嘴巴下移5像素
    
    // 嘴巴微微抖动的动画
    static lv_anim_t mouth_width_anim;
    lv_anim_init(&mouth_width_anim);
    lv_anim_set_var(&mouth_width_anim, mouth_);
    lv_anim_set_values(&mouth_width_anim, 16, 30);  // 增大幅度：更明显的开合
    lv_anim_set_time(&mouth_width_anim, 550);       // 拉长时长以更平滑
    lv_anim_set_exec_cb(&mouth_width_anim, [](void* obj, int32_t value) {
        lv_obj_set_width((lv_obj_t*)obj, value);
        // 重新居中，使用屏幕宽度160而不是硬编码240
        lv_obj_set_x((lv_obj_t*)obj, (160 - value) / 2);
    });
    lv_anim_set_path_cb(&mouth_width_anim, lv_anim_path_linear); // 线性路径更稳定
    lv_anim_set_repeat_count(&mouth_width_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_width_anim, 600);
    lv_anim_start(&mouth_width_anim);
}

void EyeDisplayHorizontal::StartThinkingAnimation() {
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
    lv_obj_set_pos(left_hand_, 15, height_ - 40 - DISPLAY_VERTICAL_OFFSET);  // 适配横屏

    // 创建右手图片
    right_hand_ = lv_img_create(lv_scr_act());
    lv_img_set_src(right_hand_, &hand_right_img);
    lv_obj_set_style_img_recolor(right_hand_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(right_hand_, LV_OPA_COVER, 0);
    // 设置右手位置
    lv_obj_set_pos(right_hand_, width_ - 55, height_ - 40 - DISPLAY_VERTICAL_OFFSET);  // 适配横屏
    // 左手左右移动动画
    static lv_anim_t left_hand_anim;
    lv_anim_init(&left_hand_anim);
    lv_anim_set_var(&left_hand_anim, left_hand_);
    lv_anim_set_values(&left_hand_anim, 30, 45);  // 适配横屏
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
    lv_anim_set_values(&right_hand_anim, width_ - 55, width_ - 75);  // 适配横屏
    lv_anim_set_time(&right_hand_anim, 1000);
    lv_anim_set_delay(&right_hand_anim, 0);
    lv_anim_set_exec_cb(&right_hand_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&right_hand_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_hand_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_hand_anim, 1000);
    lv_anim_set_playback_delay(&right_hand_anim, 0);
    lv_anim_start(&right_hand_anim);
}

void EyeDisplayHorizontal::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Create a container for eyes - 适配128x160横屏显示
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);  // 确保容器居中
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // 设置容器的顶部内边距来实现向上偏移 - 适配横屏
    lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET, 0);

    // Create left eye - 适配横屏尺寸
    left_eye_ = lv_obj_create(container);
    lv_obj_set_size(left_eye_, 30, 60);  // 缩小尺寸适配横屏
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_eye_, 0, 0);
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);
    lv_obj_set_style_outline_width(left_eye_, 0, 0);

    // Create right eye - 适配横屏尺寸
    right_eye_ = lv_obj_create(container);
    lv_obj_set_size(right_eye_, 30, 60);  // 缩小尺寸适配横屏
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_eye_, 0, 0);
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);
    lv_obj_set_style_outline_width(right_eye_, 0, 0);

    // 启动默认的待机动画
    StartIdleAnimation();
}

void EyeDisplayHorizontal::TestNextEmotion() {
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
    ESP_LOGI(TAG, "按键切换表情 %zu/%zu: %s", current_index + 1, emotion_count, emotion);
    
    // 设置表情
    SetEmotion(emotion);
    
    // 移动到下一个表情
    current_index = (current_index + 1) % emotion_count;
} 

void EyeDisplayHorizontal::EnterWifiConfig() {
    ESP_LOGI(TAG, "EnterWifiConfig");
    if (qrcode_img_) {
        ESP_LOGI(TAG, "EnterWifiConfig qrcode_img_ is not null");
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
            lv_obj_set_style_img_recolor(img, lv_color_hex(EYE_COLOR), 0);
            lv_obj_center(img);
        }
    }
}

void EyeDisplayHorizontal::EnterOTAMode() {
    ESP_LOGI(TAG, "EnterOTAMode");
    
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

void EyeDisplayHorizontal::SetOTAProgress(int progress) {
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