#include "eye_display_horizontal_emojis.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>
#include "application.h"
#include <math.h>

#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

#define TAG "EyeDisplayHorizontalEmo"

void EyeDisplayHorizontalEmo::ClearEmojiScene() {
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

    // 清理可能存在的高兴表情对象
    if (face_) {
        lv_obj_del(face_);
        face_ = nullptr;
        happy_left_eye_ = nullptr;
        happy_right_eye_ = nullptr;
        happy_mouth_ = nullptr;
        happy_mouth_mask_ = nullptr;
    }

    // 清理可能存在的独立嘴巴对象（如 loving 表情在 screen 下创建的）
    if (happy_mouth_) {
        lv_obj_del(happy_mouth_);
        happy_mouth_ = nullptr;
    }

    // 清理可能存在的震惊表情对象
    if (shocked_face_) {
        lv_obj_del(shocked_face_);
        shocked_face_ = nullptr;
        shocked_left_eye_ = nullptr;
        shocked_right_eye_ = nullptr;
        shocked_left_eyebrow_ = nullptr;
        shocked_right_eyebrow_ = nullptr;
        shocked_mouth_ = nullptr;
        shocked_line1_ = nullptr;
        shocked_line2_ = nullptr;
        shocked_line3_ = nullptr;
    }

    // 清理可能存在的震惊嘴巴对象
    if (shocked_mouth_) {
        lv_obj_del(shocked_mouth_);
        shocked_mouth_ = nullptr;
    }

    // neutral face动画使用静态对象，在ClearEmojiScene中不需要清理
    // 动画会在下次调用StartNeutralFaceAnimation时自动停止
}

EyeDisplayHorizontalEmo::EyeDisplayHorizontalEmo(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    const lv_img_dsc_t* qrcode_img,
    DisplayFonts fonts)
: EyeDisplayHorizontalEmo(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy, fonts) // 委托构造
{
    qrcode_img_ = qrcode_img;
}


EyeDisplayHorizontalEmo::EyeDisplayHorizontalEmo(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
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
            .mirror_y = false,
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

EyeDisplayHorizontalEmo::~EyeDisplayHorizontalEmo() {
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

bool EyeDisplayHorizontalEmo::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EyeDisplayHorizontalEmo::Unlock() {
    lvgl_port_unlock();
}

void EyeDisplayHorizontalEmo::SetEmotion(const char* emotion) {
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

void EyeDisplayHorizontalEmo::EmotionTask(void* arg) {
    EyeDisplayHorizontalEmo* display = static_cast<EyeDisplayHorizontalEmo*>(arg);
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

void EyeDisplayHorizontalEmo::ProcessEmotionChange(const char* emotion) {
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
    }else if (strcmp(emotion, "happy_face") == 0) {  // 添加开心表情的判断
        new_state = EyeState::HAPPY_FACE;
    } else if (strcmp(emotion, "sad_emoji") == 0) {  // 添加悲伤表情的判断
        new_state = EyeState::SAD_EMOJI;
    } else if (strcmp(emotion, "shocked_emoji") == 0) {  // 添加震惊表情的判断
        new_state = EyeState::SHOCKED_EMOJI;
    } else if (strcmp(emotion, "star_loving") == 0) {  // 添加喜爱表情的判断
        new_state = EyeState::STAR_LOVING;
    } else if (strcmp(emotion, "neutral_face") == 0) {  // 添加中性表情的判断
        new_state = EyeState::NEUTRAL_FACE;
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

    // 统一清屏清对象
    ClearEmojiScene();

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
            case EyeState::HAPPY_FACE:  // 添加新表情动画
            StartHappyFaceAnimation();
            break;
        case EyeState::NEUTRAL_FACE:  // 添加中性表情动画
            StartNeutralFaceAnimation();
            break;
        case EyeState::SAD_EMOJI:  // 添加悲伤表情动画
            StartSadEmojiAnimation();
            break;
        case EyeState::SHOCKED_EMOJI:  // 添加震惊表情动画
            StartShockedEmojiAnimation();
            break;
        case EyeState::STAR_LOVING:  // 添加喜爱表情动画
            StartLovingEmojiAnimation();
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
                    EyeDisplayHorizontalEmo* self = static_cast<EyeDisplayHorizontalEmo*>(arg);
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

void EyeDisplayHorizontalEmo::StartIdleAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 次重新设置位置，上移 20
    lv_obj_t* container = lv_obj_get_parent(left_eye_);
    if (container) {
        lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 20, 0);
    }
    
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

void EyeDisplayHorizontalEmo::StartHappyAnimation() {
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
    lv_obj_set_pos(mouth_, (width_ - 36) / 2, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 15);  // 适配横屏，happy表情嘴巴往下调15像素（10+5）
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

void EyeDisplayHorizontalEmo::StartSadAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    // 禁用屏幕滚动与滚动条，保留眼泪上下浮动但不出现滚动条
    {
        lv_obj_t* screen = lv_screen_active();
        if (screen) {
            lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        }
    }
    // 禁用容器滚动与滚动条，避免容器上下移动
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
        }
    }
    // 将眼睛整体上移30像素（在原基础上再上移20）
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 30, 0);
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
    lv_obj_set_pos(mouth_, (width_ - 28) / 2, height_ - 40 - DISPLAY_VERTICAL_OFFSET + 30);  // sad表情：整体上移20像素
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
    // 禁用眼泪对象滚动与滚动条
    lv_obj_clear_flag(right_tear_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right_tear_, LV_SCROLLBAR_MODE_OFF);

    // 将眼泪对齐到右眼下方（相对于左眼基准向右偏移60像素）
    lv_obj_align_to(right_tear_, left_eye_, LV_ALIGN_OUT_BOTTOM_MID, 100, 0);
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


void EyeDisplayHorizontalEmo::StartVertigoAnimation() {
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


void EyeDisplayHorizontalEmo::StartLovingAnimation() {
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

void EyeDisplayHorizontalEmo::StartSleepingAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 眼睛整体下移15像素
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET + 15, 0);
        }
    }
    
    // 设置眼睛为水平长条
    lv_obj_set_size(left_eye_, 45, 15);  // 适配横屏尺寸
    lv_obj_set_size(right_eye_, 45, 15);  // 适配横屏尺寸
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);

    // 创建三个 z 标签（缩小尺寸、缩短左右间距）
    zzz1_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz1_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz1_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz1_, "z");
    lv_obj_align(zzz1_, LV_ALIGN_TOP_MID, -30, 50 - DISPLAY_VERTICAL_OFFSET);  // 左侧更靠近中心
    lv_obj_set_style_text_letter_space(zzz1_, 1, 0);  // 缩短字间距
    lv_obj_set_style_transform_zoom(zzz1_, 205, 0);   // 约0.8x缩放（256为1.0）

    zzz2_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz2_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz2_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz2_, "z");
    lv_obj_align(zzz2_, LV_ALIGN_TOP_MID, 0, 40 - DISPLAY_VERTICAL_OFFSET);   // 中间
    lv_obj_set_style_text_letter_space(zzz2_, 1, 0);  // 缩短字间距
    lv_obj_set_style_transform_zoom(zzz2_, 205, 0);   // 约0.8x缩放

    zzz3_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz3_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz3_, lv_color_hex(EYE_COLOR), 0);  // 黄色
    lv_label_set_text(zzz3_, "z");
    lv_obj_align(zzz3_, LV_ALIGN_TOP_MID, 30, 30 - DISPLAY_VERTICAL_OFFSET);  // 右侧更靠近中心
    lv_obj_set_style_text_letter_space(zzz3_, 1, 0);  // 缩短字间距
    lv_obj_set_style_transform_zoom(zzz3_, 205, 0);   // 约0.8x缩放
}

void EyeDisplayHorizontalEmo::StartShockedAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 每次进入都重设容器位置（上移 20）
    lv_obj_t* container = lv_obj_get_parent(left_eye_);
    if (container) {
        lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 20, 0);
    }

    // 清理旧动画与旧对象，避免叠加影响
    lv_anim_del(left_eye_, nullptr);
    lv_anim_del(right_eye_, nullptr);
    if (mouth_) {
        lv_obj_del(mouth_);
        mouth_ = nullptr;
    }

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
    lv_obj_set_pos(mouth_, (width_ - 34) / 2, height_ - 70 - DISPLAY_VERTICAL_OFFSET + 45);  // 居中显示，向下移动30像素
    
    // 为嘴巴添加大小动画，模拟震惊的效果
    static lv_anim_t mouth_size_anim;
    lv_anim_init(&mouth_size_anim);
    lv_anim_set_var(&mouth_size_anim, mouth_);
    lv_anim_set_values(&mouth_size_anim, 20, 35);  // 从小到大变化
    lv_anim_set_time(&mouth_size_anim, 800);
    lv_anim_set_exec_cb(&mouth_size_anim, [](void* obj, int32_t value) {
        lv_obj_t* o = (lv_obj_t*)obj;
        lv_obj_set_size(o, value, value);
        // 重新居中：使用配置的屏幕水平分辨率
        lv_obj_set_x(o, ((int)LV_HOR_RES - value) / 2);
    });
    lv_anim_set_path_cb(&mouth_size_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_size_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_size_anim, 800);
    lv_anim_start(&mouth_size_anim);
}

void EyeDisplayHorizontalEmo::StartSillyAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    // 进入时刷新位置，上移 20 像素
    lv_obj_t* container = lv_obj_get_parent(left_eye_);
    if (container) {
        lv_obj_set_style_pad_top(container, -DISPLAY_VERTICAL_OFFSET - 20, 0);
    }
    
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

void EyeDisplayHorizontalEmo::StartAngryAnimation() {
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
        lv_obj_t* o = (lv_obj_t*)obj;
        lv_obj_set_width(o, value);
        // 重新居中：使用配置的屏幕水平分辨率
        lv_obj_set_x(o, ((int)LV_HOR_RES - value) / 2);
    });
    lv_anim_set_path_cb(&mouth_width_anim, lv_anim_path_linear); // 线性路径更稳定
    lv_anim_set_repeat_count(&mouth_width_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_width_anim, 600);
    lv_anim_start(&mouth_width_anim);
}


void EyeDisplayHorizontalEmo::StartThinkingAnimation() {
    // 确保眼睛可见
    lv_obj_clear_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);

    {
        lv_obj_t* screen = lv_screen_active();
        if (screen) {
            lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        }
    }
    // 禁用容器滚动与滚动条，避免容器上下移动
    {
        lv_obj_t* container = lv_obj_get_parent(left_eye_);
        if (container) {
            lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
        }
    }
    
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
    lv_obj_set_pos(mouth_, (width_ - 36) / 2, height_ - 40 - DISPLAY_VERTICAL_OFFSET);
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(mouth_, LV_OPA_COVER, 0);


    // 创建左手图片
    left_hand_ = lv_img_create(lv_scr_act());
    lv_img_set_src(left_hand_, &hand_img);
    lv_obj_set_style_img_recolor(left_hand_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(left_hand_, LV_OPA_COVER, 0);
    lv_obj_set_pos(left_hand_, 20, height_ - 20 - DISPLAY_VERTICAL_OFFSET);
    lv_img_set_zoom(left_hand_, 256 * 0.6);

    // 创建右手图片
    right_hand_ = lv_img_create(lv_scr_act());
    lv_img_set_src(right_hand_, &hand_right_img);
    lv_obj_set_style_img_recolor(right_hand_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(right_hand_, LV_OPA_COVER, 0);
    lv_img_set_zoom(right_hand_, 256 * 0.6);
    // 设置右手位置
    lv_obj_set_pos(right_hand_, width_ - 74, height_ - 20 - DISPLAY_VERTICAL_OFFSET);
    // 左手左右移动动画
    static lv_anim_t left_hand_anim;
    lv_anim_init(&left_hand_anim);
    lv_anim_set_var(&left_hand_anim, left_hand_);
    lv_anim_set_values(&left_hand_anim, 20, 40);
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
    lv_anim_set_values(&right_hand_anim, width_ - 60, width_ - 80);
    lv_anim_set_time(&right_hand_anim, 1000);
    lv_anim_set_delay(&right_hand_anim, 0);
    lv_anim_set_exec_cb(&right_hand_anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&right_hand_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_hand_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_hand_anim, 1000);
    lv_anim_set_playback_delay(&right_hand_anim, 0);
    lv_anim_start(&right_hand_anim);
}

void EyeDisplayHorizontalEmo::SetupUI() {
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

void EyeDisplayHorizontalEmo::TestNextEmotion() {
    // 定义所有表情的字符串数组，按EyeState枚举的顺序
    static const char* emotions[] = {
        "neutral",      // IDLE
        "happy",        // HAPPY
        "happy_face",   // HAPPY_FACE
        "sad_emoji",    // SAD_EMOJI
        "shocked_emoji", // SHOCKED_EMOJI
        "star_loving",   // STAR_LOVING
        "neutral_face" , // NEUTRAL_FACE
        "laughing",     // LAUGHING
        "thinking",     // THINKING
        "sad",          // SAD
        "thinking",     // THINKING
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

void EyeDisplayHorizontalEmo::EnterWifiConfig() {
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

void EyeDisplayHorizontalEmo::EnterOTAMode() {
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

void EyeDisplayHorizontalEmo::SetOTAProgress(int progress) {
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

// 开心的表情
void EyeDisplayHorizontalEmo::StartHappyFaceAnimation() {
    // 仅显示表情：隐藏默认的圆形眼睛
    
    if (left_eye_) lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    if (right_eye_) lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);

    // 清理旧对象（防御性处理）
    if (face_) {
        lv_obj_del(face_);
        face_ = nullptr;
        happy_left_eye_ = nullptr;
        happy_right_eye_ = nullptr;
        happy_mouth_ = nullptr;
        happy_mouth_mask_ = nullptr;
    }

    lv_obj_t* screen = lv_screen_active();

    // 适配不同分辨率（以240x240为基准）
    const int SCREEN_WIDTH = LV_HOR_RES;
    const int SCREEN_HEIGHT = LV_VER_RES;
    const float SCALE = (float)std::min(SCREEN_WIDTH, SCREEN_HEIGHT) / 240.0f;
    const int EYE_SIZE = (int)(40 * SCALE); 
    const int EYE_SPACING = (int)(65 * SCALE); // 眼睛间距
    const int MOUTH_SIZE = (int)(110 * SCALE); // 增大嘴巴：从96增加到110
    const int MOUTH_MASK_HEIGHT = (int)(55 * SCALE); // 相应增大遮罩高度 
    // 计算居中位置：让整个表情在屏幕正中间
    const int EYE_Y_OFFSET = (int)(-40 * SCALE); // 眼睛在中心上方35像素（增加5像素距离）
    const int MOUTH_Y_OFFSET = (int)(30 * SCALE); // 嘴巴在中心下方35像素（增加5像素距离）

    // 容器便于整体管理/动画
    face_ = lv_obj_create(screen);
    lv_obj_remove_style_all(face_);
    lv_obj_set_size(face_, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(face_, LV_OPA_TRANSP, 0);
    lv_obj_align(face_, LV_ALIGN_CENTER, 0, 0); // 容器完全居中

    // 嘴巴：使用主题色圆形 + 黑色上半遮罩，形成半圆笑嘴
    happy_mouth_ = lv_obj_create(face_);
    lv_obj_remove_style_all(happy_mouth_);
    lv_obj_set_size(happy_mouth_, MOUTH_SIZE, MOUTH_SIZE);
    lv_obj_set_style_radius(happy_mouth_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(happy_mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(happy_mouth_, LV_OPA_COVER, 0);
    lv_obj_align(happy_mouth_, LV_ALIGN_CENTER, 0, MOUTH_Y_OFFSET);

    // 黑色遮罩（覆盖圆形上半部分，留下底部半圆）
    happy_mouth_mask_ = lv_obj_create(happy_mouth_);
    lv_obj_remove_style_all(happy_mouth_mask_);
    lv_obj_set_size(happy_mouth_mask_, MOUTH_SIZE, MOUTH_MASK_HEIGHT);
    lv_obj_set_style_bg_color(happy_mouth_mask_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(happy_mouth_mask_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(happy_mouth_mask_, 0, 0);
    lv_obj_set_style_pad_all(happy_mouth_mask_, 0, 0);
    lv_obj_set_style_shadow_width(happy_mouth_mask_, 0, 0);
    lv_obj_set_style_outline_width(happy_mouth_mask_, 0, 0);
    lv_obj_clear_flag(happy_mouth_mask_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(happy_mouth_mask_, LV_ALIGN_TOP_MID, 0, 0);

    // 左眼（圆形，使用独立的位置计算）- 放在嘴巴后面创建，确保在嘴巴上面
    happy_left_eye_ = lv_obj_create(face_);
    lv_obj_remove_style_all(happy_left_eye_);
    lv_obj_set_size(happy_left_eye_, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(happy_left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(happy_left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(happy_left_eye_, LV_OPA_COVER, 0);
    lv_obj_align(happy_left_eye_, LV_ALIGN_LEFT_MID, EYE_SPACING, EYE_Y_OFFSET);

    // 右眼（圆形）- 放在嘴巴后面创建，确保在嘴巴上面
    happy_right_eye_ = lv_obj_create(face_);
    lv_obj_remove_style_all(happy_right_eye_);
    lv_obj_set_size(happy_right_eye_, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(happy_right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(happy_right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(happy_right_eye_, LV_OPA_COVER, 0);
    lv_obj_align(happy_right_eye_, LV_ALIGN_RIGHT_MID, -EYE_SPACING, EYE_Y_OFFSET);


    // 定义动画常量
    const int MOUTH_ANIM_OFFSET = 8;
    const int MOUTH_ANIM_TIME = 900;
    const int EYE_ANIM_MIN_SIZE = 33; // 减小5：36-5=31
    const int EYE_ANIM_MAX_SIZE = 38; // 减小5：44-5=39
    const int EYE_ANIM_TIME = MOUTH_ANIM_TIME; // 与嘴巴动画时长同步

    // 简单动画：嘴巴轻微上下跳动，双眼轻微缩放
    lv_anim_init(&mouth_anim_);
    lv_anim_set_var(&mouth_anim_, happy_mouth_);
    // 使用相对位置进行动画
    lv_anim_set_values(&mouth_anim_, MOUTH_Y_OFFSET, MOUTH_Y_OFFSET - MOUTH_ANIM_OFFSET);
    lv_anim_set_time(&mouth_anim_, MOUTH_ANIM_TIME);
    lv_anim_set_exec_cb(&mouth_anim_, [](void* obj, int32_t v){
        lv_obj_t* mouth = (lv_obj_t*)obj;
        lv_obj_align(mouth, LV_ALIGN_CENTER, 0, v); // 保持水平居中，只改变垂直位置
    });
    lv_anim_set_path_cb(&mouth_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&mouth_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim_, MOUTH_ANIM_TIME);
    lv_anim_start(&mouth_anim_);


    static lv_anim_t left_eye_anim;
    lv_anim_init(&left_eye_anim);
    lv_anim_set_var(&left_eye_anim, happy_left_eye_);
    lv_anim_set_values(&left_eye_anim, EYE_ANIM_MIN_SIZE, EYE_ANIM_MAX_SIZE);
    lv_anim_set_time(&left_eye_anim, EYE_ANIM_TIME);
    lv_anim_set_exec_cb(&left_eye_anim, [](void* obj, int32_t v){
        lv_obj_t* eye = (lv_obj_t*)obj;
        lv_obj_set_size(eye, v, v);  // 等比缩放，保持圆形
    });
    lv_anim_set_path_cb(&left_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim, EYE_ANIM_TIME);
    lv_anim_start(&left_eye_anim);

    static lv_anim_t right_eye_anim;
    lv_anim_init(&right_eye_anim);
    lv_anim_set_var(&right_eye_anim, happy_right_eye_);
    lv_anim_set_values(&right_eye_anim, EYE_ANIM_MIN_SIZE, EYE_ANIM_MAX_SIZE);
    lv_anim_set_time(&right_eye_anim, EYE_ANIM_TIME);
    lv_anim_set_exec_cb(&right_eye_anim, [](void* obj, int32_t v){
        lv_obj_t* eye = (lv_obj_t*)obj;
        lv_obj_set_size(eye, v, v);  // 等比缩放，保持圆形
    });
    lv_anim_set_path_cb(&right_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim, EYE_ANIM_TIME);
    lv_anim_start(&right_eye_anim);

}

// 悲伤的表情
void EyeDisplayHorizontalEmo::StartSadEmojiAnimation() {
    // 隐藏原来的圆形眼睛
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* screen = lv_screen_active();
    
    // 适配不同分辨率（以240x240为基准）
    const int SCREEN_WIDTH = LV_HOR_RES;
    const int SCREEN_HEIGHT = LV_VER_RES;
    const float SCALE = (float)std::min(SCREEN_WIDTH, SCREEN_HEIGHT) / 240.0f;
    const int EYE_SIZE = (int)(55 * SCALE);      // 眼睛大小增大到50
    const int EYE_SPACING = (int)(60 * SCALE);    // 眼睛间距缩短40：40-40=0
    // 眉毛间距（当前未用）
    // int EYEBROW_SPACING = 35;
    const int TEAR_SIZE = 4;       // 眼泪大小缩小一半：8/2=4
    const int VERTICAL_OFFSET = 20; // 垂直偏移量，缩短10：10+10=20
    
    // 创建面部容器
    lv_obj_t* face = lv_obj_create(screen);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, 0);
    lv_obj_align(face, LV_ALIGN_CENTER, 0, 0); // 容器完全居中
    
    // 左眼（圆形）
    lv_obj_t* left_eye = lv_obj_create(face);
    lv_obj_remove_style_all(left_eye);
    lv_obj_set_size(left_eye, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(left_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_eye, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye, LV_OPA_COVER, 0);
    lv_obj_align(left_eye, LV_ALIGN_LEFT_MID, EYE_SPACING, -20); // 眼睛在中心上方20像素（往下移动10）
    
    // 右眼（圆形）
    lv_obj_t* right_eye = lv_obj_create(face);
    lv_obj_remove_style_all(right_eye);
    lv_obj_set_size(right_eye, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(right_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_eye, LV_OPA_COVER, 0);
    lv_obj_align(right_eye, LV_ALIGN_RIGHT_MID, -EYE_SPACING, -20); // 眼睛在中心上方20像素（往下移动10）
    
    // 左眉毛（斜向下弯曲的弧形）
    const int EYEBROW_ARC_SIZE = 30;  // 眉毛弧形大小缩小一半：60/2=30
    lv_obj_t* left_eyebrow = lv_arc_create(face);
    lv_obj_set_size(left_eyebrow, EYEBROW_ARC_SIZE, EYEBROW_ARC_SIZE);
    // 左眉毛与左眼同心（向左侧同心）：水平偏移与左眼一致，再向左移动10像素
    lv_obj_align(left_eyebrow, LV_ALIGN_LEFT_MID, EYE_SPACING-3, -25); // 眉毛再往上移动15像素
    lv_arc_set_bg_angles(left_eyebrow, 204, 294);              // 眉毛：左边下垂，右边上翘，保持90度长度
    lv_arc_set_rotation(left_eyebrow, 0);                      // 角度基准不旋转
    lv_arc_set_value(left_eyebrow, 100);                       // 指示弧覆盖整个弧
    lv_obj_remove_style(left_eyebrow, NULL, LV_PART_KNOB);     // 移除旋钮
    lv_obj_clear_flag(left_eyebrow, LV_OBJ_FLAG_CLICKABLE);    // 不可点击
    // 背景弧隐藏到黑色背景
    lv_obj_set_style_arc_color(left_eyebrow, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(left_eyebrow, 4, LV_PART_MAIN);  // 眉毛厚度缩小一半：8/2=4
    // 指示弧使用主题色，形成可见弧线
    lv_obj_set_style_arc_color(left_eyebrow, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(left_eyebrow, 4, LV_PART_INDICATOR);  // 眉毛厚度缩小一半：8/2=4
    
    // 右眉毛（斜向下弯曲的弧形）
    lv_obj_t* right_eyebrow = lv_arc_create(face);
    lv_obj_set_size(right_eyebrow, EYEBROW_ARC_SIZE, EYEBROW_ARC_SIZE);
    // 右眉毛与右眼同心（向右侧同心）：水平偏移与右眼一致，再向右移动10像素
    lv_obj_align(right_eyebrow, LV_ALIGN_RIGHT_MID, -EYE_SPACING + 3, -25); // 与左眉毛镜像位置
    lv_arc_set_bg_angles(right_eyebrow, 240, 330);              // 左眉毛的镜像角度，保持90度长度
    lv_arc_set_rotation(right_eyebrow, 0);                      // 角度基准不旋转
    lv_arc_set_value(right_eyebrow, 100);                       // 指示弧覆盖整个弧
    lv_obj_remove_style(right_eyebrow, NULL, LV_PART_KNOB);     // 移除旋钮
    lv_obj_clear_flag(right_eyebrow, LV_OBJ_FLAG_CLICKABLE);    // 不可点击
    // 背景弧隐藏到黑色背景
    lv_obj_set_style_arc_color(right_eyebrow, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(right_eyebrow, 4, LV_PART_MAIN);  // 眉毛厚度缩小一半：8/2=4
    // 指示弧使用主题色，形成可见弧线
    lv_obj_set_style_arc_color(right_eyebrow, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(right_eyebrow, 4, LV_PART_INDICATOR);  // 眉毛厚度缩小一半：8/2=4
    
    // 嘴巴（反向、中空弧形：使用圆弧控件绘制上半圆）
    const int MOUTH_ARC_SIZE = 53;  // 嘴巴大小缩小一半：88/2=44
    lv_obj_t* mouth = lv_arc_create(face);
    lv_obj_set_size(mouth, MOUTH_ARC_SIZE, MOUTH_ARC_SIZE);
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 55); // 嘴巴在中心下方55像素（往下移动10）
    lv_arc_set_bg_angles(mouth, 210, 330);              // 背景弧：上方约1/3圆
    lv_arc_set_rotation(mouth, 0);                      // 角度基准不旋转
    lv_arc_set_value(mouth, 100);                       // 指示弧覆盖整个上半圆
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);     // 移除旋钮
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_CLICKABLE);    // 不可点击
    // 背景弧隐藏到黑色背景
    lv_obj_set_style_arc_color(mouth, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth, 6, LV_PART_MAIN);  // 嘴巴厚度缩小一半：12/2=6
    // 指示弧使用主题色，形成可见弧线
    lv_obj_set_style_arc_color(mouth, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(mouth, 7, LV_PART_INDICATOR);  // 嘴巴厚度缩小一半：12/2=6
    
    // 创建2个眼泪对象，实现连续下落效果
    lv_obj_t* right_tear1 = lv_obj_create(face);
    lv_obj_remove_style_all(right_tear1);
    lv_obj_set_size(right_tear1, TEAR_SIZE, TEAR_SIZE * 2);
    lv_obj_set_style_radius(right_tear1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear1, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_tear1, LV_OPA_COVER, 0);
    lv_obj_align(right_tear1, LV_ALIGN_RIGHT_MID, -EYE_SPACING + 5, 10); // 眼泪在中心下方10像素（往下移动10）
    
    lv_obj_t* right_tear2 = lv_obj_create(face);
    lv_obj_remove_style_all(right_tear2);
    lv_obj_set_size(right_tear2, TEAR_SIZE, TEAR_SIZE * 2);
    lv_obj_set_style_radius(right_tear2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear2, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_tear2, LV_OPA_COVER, 0);
    lv_obj_align(right_tear2, LV_ALIGN_LEFT_MID, EYE_SPACING - 5, 10);  // 左眼泪在中心下方10像素（往下移动10）
    
    // 保存对象指针以便清理
    face_ = face;
    happy_left_eye_ = left_eye;
    happy_right_eye_ = right_eye;
    happy_mouth_ = mouth;
    happy_mouth_mask_ = nullptr;  // 不使用遮罩
    
    // 设置眼泪初始透明度为0（完全透明）
    lv_obj_set_style_bg_opa(right_tear1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(right_tear2, LV_OPA_TRANSP, 0);
    
    // 重新设计眼泪动画：与眼睛、嘴巴动画同步（2秒完整循环）
    // 眼泪动画：2秒一个周期（淡入-保持-淡出），与其他部件节奏一致
    
    // 右眼泪动画：同时控制透明度和位置
    static lv_anim_t right_tear_anim;
    lv_anim_init(&right_tear_anim);
    lv_anim_set_var(&right_tear_anim, right_tear1);
    lv_anim_set_values(&right_tear_anim, 0, 100);  // 0-100 表示动画进度
    lv_anim_set_time(&right_tear_anim, 2000);  // 2秒一个完整周期
    lv_anim_set_delay(&right_tear_anim, 0);
    lv_anim_set_repeat_count(&right_tear_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_tear_anim, 0);
    lv_anim_set_exec_cb(&right_tear_anim, [](void* obj, int32_t value) {
        lv_obj_t* tear = (lv_obj_t*)obj;
        int32_t progress = value;  // 0-100
        
        // 计算Y位置：从起始位置到终点位置（起始位置整体上移30，再下移5）
        int32_t start_y = -15; // 上移30再下移5：10 -> -20 -> -15
        int32_t end_y = 45;    // 同步下移5：40 -> 45
        int32_t current_y = start_y + (end_y - start_y) * progress / 100;
        lv_obj_set_y(tear, current_y);
        
        // 计算透明度：2秒周期内（0-100）→ 0-20%淡入，20-80%保持，80-100%淡出
        lv_opa_t opacity = LV_OPA_TRANSP;
        if (progress <= 20) {  // 0-20%: 淡入
            opacity = LV_OPA_TRANSP + (LV_OPA_COVER - LV_OPA_TRANSP) * progress / 20;
        } else if (progress <= 80) {  // 20-80%: 保持不透明
            opacity = LV_OPA_COVER;
        } else {  // 80-100%: 淡出
            opacity = LV_OPA_COVER - (LV_OPA_COVER - LV_OPA_TRANSP) * (progress - 80) / 20;
        }
        lv_obj_set_style_bg_opa(tear, opacity, 0);
    });
    lv_anim_set_path_cb(&right_tear_anim, lv_anim_path_linear);
    lv_anim_start(&right_tear_anim);
    
    // 左眼泪动画：与右眼泪同步出现
    static lv_anim_t left_tear_anim;
    lv_anim_init(&left_tear_anim);
    lv_anim_set_var(&left_tear_anim, right_tear2);  // 使用第二颗眼泪作为左眼泪
    lv_anim_set_values(&left_tear_anim, 0, 100);  // 0-100 表示动画进度
    lv_anim_set_time(&left_tear_anim, 2000);  // 2秒一个完整周期
    lv_anim_set_delay(&left_tear_anim, 0);  // 与右眼泪同步开始
    lv_anim_set_repeat_count(&left_tear_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_tear_anim, 0);
    lv_anim_set_exec_cb(&left_tear_anim, [](void* obj, int32_t value) {
        lv_obj_t* tear = (lv_obj_t*)obj;
        int32_t progress = value;  // 0-100
        
        // 计算Y位置：从起始位置到终点位置（与右眼泪相同，起始位置整体上移30，再下移5）
        int32_t start_y = -15; // 上移30再下移5：10 -> -20 -> -15
        int32_t end_y = 45;    // 同步下移5：40 -> 45
        int32_t current_y = start_y + (end_y - start_y) * progress / 100;
        lv_obj_set_y(tear, current_y);
        
        // 计算透明度（与右眼泪相同）：2秒周期内 0-20%淡入，20-80%保持，80-100%淡出
        lv_opa_t opacity = LV_OPA_TRANSP;
        if (progress <= 20) {  // 0-20%: 淡入
            opacity = LV_OPA_TRANSP + (LV_OPA_COVER - LV_OPA_TRANSP) * progress / 20;
        } else if (progress <= 80) {  // 20-80%: 保持不透明
            opacity = LV_OPA_COVER;
        } else {  // 80-100%: 淡出
            opacity = LV_OPA_COVER - (LV_OPA_COVER - LV_OPA_TRANSP) * (progress - 80) / 20;
        }
        lv_obj_set_style_bg_opa(tear, opacity, 0);
    });
    lv_anim_set_path_cb(&left_tear_anim, lv_anim_path_linear);
    lv_anim_start(&left_tear_anim);
    
    // 显示左眼泪（之前被隐藏的第二颗眼泪）
    lv_obj_clear_flag(right_tear2, LV_OBJ_FLAG_HIDDEN);
    
    // 添加眉毛轻微抖动动画
    static lv_anim_t left_eyebrow_anim;
    lv_anim_init(&left_eyebrow_anim);
    lv_anim_set_var(&left_eyebrow_anim, left_eyebrow);
    lv_anim_set_values(&left_eyebrow_anim, -26, -30); // 最低点上调3、最高点上调2
    lv_anim_set_time(&left_eyebrow_anim, 2000);
    lv_anim_set_repeat_count(&left_eyebrow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eyebrow_anim, 2000);
    lv_anim_set_exec_cb(&left_eyebrow_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&left_eyebrow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&left_eyebrow_anim);
    
    static lv_anim_t right_eyebrow_anim;
    lv_anim_init(&right_eyebrow_anim);
    lv_anim_set_var(&right_eyebrow_anim, right_eyebrow);
    lv_anim_set_values(&right_eyebrow_anim, -26, -30); // 与左眉毛相同的动画范围
    lv_anim_set_time(&right_eyebrow_anim, 2000);
    lv_anim_set_repeat_count(&right_eyebrow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eyebrow_anim, 2000);
    lv_anim_set_exec_cb(&right_eyebrow_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&right_eyebrow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&right_eyebrow_anim);
    
    // 添加嘴巴上下移动动画，与眉毛同步
    static lv_anim_t mouth_anim;
    lv_anim_init(&mouth_anim);
    lv_anim_set_var(&mouth_anim, mouth);
    lv_anim_set_values(&mouth_anim, 50, 42); // 嘴巴动画整体上移5：55->50, 47->42
    lv_anim_set_time(&mouth_anim, 2000);
    lv_anim_set_repeat_count(&mouth_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim, 2000);
    lv_anim_set_exec_cb(&mouth_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&mouth_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&mouth_anim);
}

// 震惊的表情
void EyeDisplayHorizontalEmo::StartShockedEmojiAnimation() {
    // 隐藏原来的圆形眼睛
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* screen = lv_screen_active();
    
    // 适配不同分辨率（以240x240为基准）
    const int SCREEN_WIDTH = LV_HOR_RES;
    const int SCREEN_HEIGHT = LV_VER_RES;
    const float SCALE = (float)std::min(SCREEN_WIDTH, SCREEN_HEIGHT) / 240.0f;
    const int EYE_SIZE = (int)(30 * SCALE);             // 缩小三分之一（2/3）
    const int EYE_SPACING = (int)(57 * SCALE);          // 将原间距缩短一半（约 43 -> 21）
    const int EYEBROW_SPACING = (int)(22 * SCALE);      // 33 -> 22
    const int VERTICAL_OFFSET = (int)(17 * SCALE);      // 再下移5：22 -> 17
    
    // 创建面部容器
    shocked_face_ = lv_obj_create(screen);
    lv_obj_remove_style_all(shocked_face_);
    lv_obj_set_size(shocked_face_, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(shocked_face_, LV_OPA_TRANSP, 0);
    lv_obj_align(shocked_face_, LV_ALIGN_CENTER, 0, -VERTICAL_OFFSET);
    
    // 左眼（圆形）
    shocked_left_eye_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_left_eye_);
    lv_obj_set_size(shocked_left_eye_, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(shocked_left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shocked_left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_left_eye_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_left_eye_, LV_ALIGN_LEFT_MID, EYE_SPACING, -VERTICAL_OFFSET+2); // 下移 5 像素（-3 -> +2）
    
    // 右眼（圆形）
    shocked_right_eye_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_right_eye_);
    lv_obj_set_size(shocked_right_eye_, EYE_SIZE, EYE_SIZE);
    lv_obj_set_style_radius(shocked_right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shocked_right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_right_eye_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_right_eye_, LV_ALIGN_RIGHT_MID, -EYE_SPACING, -VERTICAL_OFFSET+2);
    
    // 左眉毛（向上翘的弧形）
    const int EYEBROW_ARC_SIZE = 53;  // 80 -> 53 (≈2/3)
    shocked_left_eyebrow_ = lv_arc_create(shocked_face_);
    lv_obj_set_size(shocked_left_eyebrow_, EYEBROW_ARC_SIZE, EYEBROW_ARC_SIZE);
    lv_obj_align(shocked_left_eyebrow_, LV_ALIGN_LEFT_MID, EYEBROW_SPACING + 23, -VERTICAL_OFFSET - 98); // 间距各内收10
    // 使用样式平移，避免布局实时对齐覆盖
    lv_obj_set_style_translate_y(shocked_left_eyebrow_, -50, 0);
    lv_arc_set_bg_angles(shocked_left_eyebrow_, 0, 0);                   // 隐藏背景弧
    // 还原到之前的眉毛大小
    lv_arc_set_angles(shocked_left_eyebrow_, 115, 165);
    lv_arc_set_rotation(shocked_left_eyebrow_, -27); // 整体逆时针旋转27度
    lv_obj_remove_style(shocked_left_eyebrow_, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(shocked_left_eyebrow_, LV_OBJ_FLAG_CLICKABLE);
    // 隐藏背景弧，只显示指示弧
    lv_obj_set_style_arc_color(shocked_left_eyebrow_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(shocked_left_eyebrow_, 0, LV_PART_MAIN);  // 背景弧宽度设为0
    lv_obj_set_style_arc_color(shocked_left_eyebrow_, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(shocked_left_eyebrow_, 5, LV_PART_INDICATOR); // 线宽 8 -> 5

    // 右眉毛（向上翘的弧形）
    shocked_right_eyebrow_ = lv_arc_create(shocked_face_);
    lv_obj_set_size(shocked_right_eyebrow_, EYEBROW_ARC_SIZE, EYEBROW_ARC_SIZE);
    lv_obj_align(shocked_right_eyebrow_, LV_ALIGN_RIGHT_MID, -EYEBROW_SPACING - 23, -VERTICAL_OFFSET - 98); // 间距各内收10
    lv_obj_set_style_translate_y(shocked_right_eyebrow_, -50, 0);
    lv_arc_set_bg_angles(shocked_right_eyebrow_, 0, 0);                   // 隐藏背景弧
    lv_arc_set_angles(shocked_right_eyebrow_, 15, 65);
    lv_arc_set_rotation(shocked_right_eyebrow_, 27); // 顺时针旋转27度，与左眉毛镜像
    lv_obj_remove_style(shocked_right_eyebrow_, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(shocked_right_eyebrow_, LV_OBJ_FLAG_CLICKABLE);
    // 隐藏背景弧，只显示指示弧
    lv_obj_set_style_arc_color(shocked_right_eyebrow_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(shocked_right_eyebrow_, 0, LV_PART_MAIN);  // 背景弧宽度设为0
    lv_obj_set_style_arc_color(shocked_right_eyebrow_, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(shocked_right_eyebrow_, 5, LV_PART_INDICATOR);

    // 嘴巴（椭圆形状）
    shocked_mouth_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_mouth_);
    lv_obj_set_size(shocked_mouth_, 30, 30);  // 45 -> 30
    lv_obj_set_style_radius(shocked_mouth_, LV_RADIUS_CIRCLE, 0);  // 圆形
    lv_obj_set_style_bg_color(shocked_mouth_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_mouth_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_mouth_, LV_ALIGN_CENTER, 0, 50 - VERTICAL_OFFSET);  // 75 -> 50
    
    // 三条竖线（震惊标记）- 右边长，左边短，长度减小三分之一，间距增加7，整体向左移动19向上移动10
    shocked_line1_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_line1_);
    lv_obj_set_size(shocked_line1_, 3, 9);   // 宽 4->3，高 13->9
    lv_obj_set_style_radius(shocked_line1_, 1, 0);
    lv_obj_set_style_bg_color(shocked_line1_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_line1_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_line1_, LV_ALIGN_RIGHT_MID, -EYEBROW_SPACING - 9, -VERTICAL_OFFSET - 17);  // 左移5
    
    shocked_line2_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_line2_);
    lv_obj_set_size(shocked_line2_, 3, 12);  // 宽 4->3，高 18->12
    lv_obj_set_style_radius(shocked_line2_, 1, 0);
    lv_obj_set_style_bg_color(shocked_line2_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_line2_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_line2_, LV_ALIGN_RIGHT_MID, -EYEBROW_SPACING - 4, -VERTICAL_OFFSET - 17);  // 左移5
    
    shocked_line3_ = lv_obj_create(shocked_face_);
    lv_obj_remove_style_all(shocked_line3_);
    lv_obj_set_size(shocked_line3_, 3, 17);  // 宽 4->3，高 25->17
    lv_obj_set_style_radius(shocked_line3_, 1, 0);
    lv_obj_set_style_bg_color(shocked_line3_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_line3_, LV_OPA_COVER, 0);
    lv_obj_align(shocked_line3_, LV_ALIGN_RIGHT_MID, -EYEBROW_SPACING + 0, -VERTICAL_OFFSET - 17);  // 左移5
    
    // 添加眉毛上下移动动画，与眼睛和嘴巴同步
    static lv_anim_t left_eyebrow_anim;
    int base_y_left = lv_obj_get_y(shocked_left_eyebrow_);
    lv_anim_init(&left_eyebrow_anim);
    lv_anim_set_var(&left_eyebrow_anim, shocked_left_eyebrow_);
    // 与嘴巴/双眼动画同步：周期 2000ms，往复，零延迟
    lv_anim_set_values(&left_eyebrow_anim, base_y_left, base_y_left - 8);
    lv_anim_set_time(&left_eyebrow_anim, 2000);
    lv_anim_set_delay(&left_eyebrow_anim, 0);
    lv_anim_set_repeat_count(&left_eyebrow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eyebrow_anim, 2000);
    lv_anim_set_playback_delay(&left_eyebrow_anim, 0);
    lv_anim_set_exec_cb(&left_eyebrow_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&left_eyebrow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&left_eyebrow_anim);
    
    // 右眉毛动画，与左眉毛同步
    static lv_anim_t right_eyebrow_anim;
    int base_y_right = lv_obj_get_y(shocked_right_eyebrow_);
    lv_anim_init(&right_eyebrow_anim);
    lv_anim_set_var(&right_eyebrow_anim, shocked_right_eyebrow_);
    lv_anim_set_values(&right_eyebrow_anim, base_y_right, base_y_right - 8);
    lv_anim_set_time(&right_eyebrow_anim, 2000);
    lv_anim_set_delay(&right_eyebrow_anim, 0);
    lv_anim_set_repeat_count(&right_eyebrow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eyebrow_anim, 2000);
    lv_anim_set_playback_delay(&right_eyebrow_anim, 0);
    lv_anim_set_exec_cb(&right_eyebrow_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&right_eyebrow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&right_eyebrow_anim);
    
    // 添加嘴巴张嘴动画（上下左右都变大，上下变化更多）
    static lv_anim_t mouth_anim;
    lv_anim_init(&mouth_anim);
    lv_anim_set_var(&mouth_anim, shocked_mouth_);
    lv_anim_set_values(&mouth_anim, 0, 100);  // 0表示小圆形，100表示大椭圆
    lv_anim_set_time(&mouth_anim, 2000);
    lv_anim_set_repeat_count(&mouth_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim, 2000);
    lv_anim_set_exec_cb(&mouth_anim, [](void* obj, int32_t v) {
        lv_obj_t* mouth = (lv_obj_t*)obj;
        // 缩小三分之一（2/3）后的张嘴动画
        int32_t width = 30 + (v * 8) / 100;    // 30-38
        int32_t height = 30 + (v * 13) / 100;  // 30-43
        lv_obj_set_size(mouth, width, height);
    });
    lv_anim_set_path_cb(&mouth_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&mouth_anim);
    
    // 添加左眼变大动画（保持圆形）
    static lv_anim_t left_eye_anim;
    lv_anim_init(&left_eye_anim);
    lv_anim_set_var(&left_eye_anim, shocked_left_eye_);
    lv_anim_set_values(&left_eye_anim, 0, 100);  // 0表示原始大小，100表示最大
    lv_anim_set_time(&left_eye_anim, 2000);
    lv_anim_set_repeat_count(&left_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim, 2000);
    lv_anim_set_exec_cb(&left_eye_anim, [](void* obj, int32_t v) {
        lv_obj_t* eye = (lv_obj_t*)obj;
        int32_t size = 30 + (v * 8) / 100;   // 30-38
        lv_obj_set_size(eye, size, size);
    });
    lv_anim_set_path_cb(&left_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&left_eye_anim);
    
    // 添加右眼变大动画（保持圆形）
    static lv_anim_t right_eye_anim;
    lv_anim_init(&right_eye_anim);
    lv_anim_set_var(&right_eye_anim, shocked_right_eye_);
    lv_anim_set_values(&right_eye_anim, 0, 100);  // 0表示原始大小，100表示最大
    lv_anim_set_time(&right_eye_anim, 2000);
    lv_anim_set_repeat_count(&right_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim, 2000);
    lv_anim_set_exec_cb(&right_eye_anim, [](void* obj, int32_t v) {
        lv_obj_t* eye = (lv_obj_t*)obj;
        int32_t size = 30 + (v * 8) / 100;   // 30-38
        lv_obj_set_size(eye, size, size);
    });
    lv_anim_set_path_cb(&right_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&right_eye_anim);
    
    // 添加第一条竖线上下变大动画（最短）- 长度减小三分之一
    static lv_anim_t line1_anim;
    lv_anim_init(&line1_anim);
    lv_anim_set_var(&line1_anim, shocked_line1_);
    lv_anim_set_values(&line1_anim, 0, 100);  // 0表示原始长度，100表示最大
    lv_anim_set_time(&line1_anim, 2000);  // 与眼睛和嘴巴同步
    lv_anim_set_repeat_count(&line1_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&line1_anim, 2000);
    lv_anim_set_exec_cb(&line1_anim, [](void* obj, int32_t v) {
        lv_obj_t* line = (lv_obj_t*)obj;
        int32_t height = 9 + (v * 5) / 100;  // 9-14
        lv_obj_set_size(line, 3, height);
    });
    lv_anim_set_path_cb(&line1_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&line1_anim);
    
    // 添加第二条竖线上下变大动画（中等长度）- 长度减小三分之一
    static lv_anim_t line2_anim;
    lv_anim_init(&line2_anim);
    lv_anim_set_var(&line2_anim, shocked_line2_);
    lv_anim_set_values(&line2_anim, 0, 100);  // 0表示原始长度，100表示最大
    lv_anim_set_time(&line2_anim, 2000);  // 与眼睛和嘴巴同步
    lv_anim_set_repeat_count(&line2_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&line2_anim, 2000);
    lv_anim_set_exec_cb(&line2_anim, [](void* obj, int32_t v) {
        lv_obj_t* line = (lv_obj_t*)obj;
        int32_t height = 12 + (v * 8) / 100;  // 12-20
        lv_obj_set_size(line, 3, height);
    });
    lv_anim_set_path_cb(&line2_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&line2_anim);
    
    // 添加第三条竖线上下变大动画（最长）- 长度减小三分之一
    static lv_anim_t line3_anim;
    lv_anim_init(&line3_anim);
    lv_anim_set_var(&line3_anim, shocked_line3_);
    lv_anim_set_values(&line3_anim, 0, 100);  // 0表示原始长度，100表示最大
    lv_anim_set_time(&line3_anim, 2000);  // 与眼睛和嘴巴同步
    lv_anim_set_repeat_count(&line3_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&line3_anim, 2000);
    lv_anim_set_exec_cb(&line3_anim, [](void* obj, int32_t v) {
        lv_obj_t* line = (lv_obj_t*)obj;
        int32_t height = 17 + (v * 12) / 100;  // 17-29
        lv_obj_set_size(line, 3, height);
    });
    lv_anim_set_path_cb(&line3_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&line3_anim);
}

// 喜爱的表情
void EyeDisplayHorizontalEmo::StartLovingEmojiAnimation() {
    // 隐藏原来的圆形眼睛
    lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    // 禁用当前屏幕滚动与滚动条
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    
    // 创建左眼五角星图片
    lv_obj_t* left_star = lv_img_create(lv_screen_active());
    lv_img_set_src(left_star, &star_img_32_hor);
    lv_obj_set_style_img_recolor(left_star, lv_color_hex(EYE_COLOR), 0);  // 设置为主题色
    lv_obj_set_style_img_recolor_opa(left_star, LV_OPA_COVER, 0);  // 完全不透明
    // 设置图片抗锯齿，减少模糊
    lv_obj_set_style_img_recolor_opa(left_star, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_zoom(left_star, 256, 0);  // 初始缩放为100%
    // 以屏幕中线为基准对称摆放（保证嘴巴在中间，双星镜像）
    const int STAR_SPACING_X = 35;  // 缩小间距30（总间距-30，单侧-15）
    // 整体下移 10 像素
    lv_obj_align(left_star, LV_ALIGN_CENTER, -STAR_SPACING_X, -DISPLAY_VERTICAL_OFFSET);
    
    // 创建右眼五角星图片
    lv_obj_t* right_star = lv_img_create(lv_screen_active());
    lv_img_set_src(right_star, &star_img_32_hor);
    lv_obj_set_style_img_recolor(right_star, lv_color_hex(EYE_COLOR), 0);  // 设置为主题色
    lv_obj_set_style_img_recolor_opa(right_star, LV_OPA_COVER, 0);  // 完全不透明
    // 设置图片抗锯齿，减少模糊
    lv_obj_set_style_img_recolor_opa(right_star, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_zoom(right_star, 256, 0);  // 初始缩放为100%
    lv_obj_align(right_star, LV_ALIGN_CENTER, STAR_SPACING_X, -DISPLAY_VERTICAL_OFFSET);
    
    // 创建嘴巴图片对象
    lv_obj_t* mouth = lv_img_create(lv_screen_active());
    lv_img_set_src(mouth, &mouth_open_img);  // 使用嘴巴图片资源
    lv_obj_set_style_img_recolor(mouth, lv_color_hex(EYE_COLOR), 0);  // 设置为主题色
    lv_obj_set_style_img_recolor_opa(mouth, LV_OPA_COVER, 0);  // 完全不透明
    // 设置图片抗锯齿，减少模糊
    lv_obj_set_style_img_recolor_opa(mouth, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_transform_zoom(mouth, 256, 0);  // 初始缩放为100%
    // 嘴巴上移更多，避免下边缘裁切（80 -> 50）
    lv_obj_align(mouth, LV_ALIGN_CENTER, 0, 60 - DISPLAY_VERTICAL_OFFSET);
    
    // 保存对象指针，以便在状态切换时清理
    left_heart_ = left_star;
    right_heart_ = right_star;
    happy_mouth_ = mouth;
    
    // 左眼五角星放大缩小动画 - 使用更平滑的缩放范围
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_star);
    // 缩小整体尺寸三分之一（相对之前 450-500）
    lv_anim_set_values(&left_anim_, 90, 120);
    lv_anim_set_time(&left_anim_, 1200);        // 稍微延长动画时间，更平滑
    lv_anim_set_playback_time(&left_anim_, 1200);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_start(&left_anim_);

    // 右眼五角星放大缩小动画 - 使用更平滑的缩放范围
    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_star);
    lv_anim_set_values(&right_anim_, 90, 120);
    lv_anim_set_time(&right_anim_, 1200);        // 稍微延长动画时间，更平滑
    lv_anim_set_playback_time(&right_anim_, 1200);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_start(&right_anim_);
    
    // 嘴巴缩放动画（与五角星同步）- 使用更平滑的缩放范围
    static lv_anim_t mouth_anim;
    lv_anim_init(&mouth_anim);
    lv_anim_set_var(&mouth_anim, mouth);
    // 嘴巴缩放：从更小到原始大小（78% -> 100%）
    lv_anim_set_values(&mouth_anim, 200, 250);
    lv_anim_set_time(&mouth_anim, 1200);        // 与五角星同步
    lv_anim_set_repeat_count(&mouth_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&mouth_anim, 1200);
    lv_anim_set_exec_cb(&mouth_anim, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&mouth_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&mouth_anim);
}

// 中性的表情
void EyeDisplayHorizontalEmo::StartNeutralFaceAnimation() {
    // 隐藏默认圆形眼睛
    if (left_eye_) lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    if (right_eye_) lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);

    // 若存在之前创建的表情容器，先删除（避免叠加）
    if (face_) {
        lv_obj_del(face_);
        face_ = nullptr;
        happy_left_eye_ = nullptr;
        happy_right_eye_ = nullptr;
        happy_mouth_ = nullptr;
        happy_mouth_mask_ = nullptr;
    }

    lv_obj_t* screen = lv_screen_active();

    // 适配不同分辨率（以240x240为基准）- 参数与 StartHappyFaceAnimation 一致
    const int SCREEN_WIDTH = LV_HOR_RES;
    const int SCREEN_HEIGHT = LV_VER_RES;
    const float SCALE = (float)std::min(SCREEN_WIDTH, SCREEN_HEIGHT) / 240.0f;
    const int NEUTRAL_EYE_SIZE = (int)(40 * SCALE);
    const int NEUTRAL_EYE_SPACING = (int)(65 * SCALE);
    const int NEUTRAL_EYE_Y_OFFSET = (int)(-40 * SCALE);

    // 面部容器（局部变量）
    lv_obj_t* neutral_face = lv_obj_create(screen);
    lv_obj_remove_style_all(neutral_face);
    lv_obj_set_size(neutral_face, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_opa(neutral_face, LV_OPA_TRANSP, 0);
    // 与 StartHappyFaceAnimation 保持容器完全居中（不再上移）
    lv_obj_align(neutral_face, LV_ALIGN_CENTER, 0, 0);

    // 左右眼（主题色圆形，局部变量）
    lv_obj_t* neutral_left_eye = lv_obj_create(neutral_face);
    lv_obj_remove_style_all(neutral_left_eye);
    lv_obj_set_size(neutral_left_eye, NEUTRAL_EYE_SIZE, NEUTRAL_EYE_SIZE);
    lv_obj_set_style_radius(neutral_left_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(neutral_left_eye, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(neutral_left_eye, LV_OPA_COVER, 0);
    lv_obj_align(neutral_left_eye, LV_ALIGN_CENTER, -NEUTRAL_EYE_SPACING, NEUTRAL_EYE_Y_OFFSET);

    lv_obj_t* neutral_right_eye = lv_obj_create(neutral_face);
    lv_obj_remove_style_all(neutral_right_eye);
    lv_obj_set_size(neutral_right_eye, NEUTRAL_EYE_SIZE, NEUTRAL_EYE_SIZE);
    lv_obj_set_style_radius(neutral_right_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(neutral_right_eye, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(neutral_right_eye, LV_OPA_COVER, 0);
    lv_obj_align(neutral_right_eye, LV_ALIGN_CENTER, NEUTRAL_EYE_SPACING, NEUTRAL_EYE_Y_OFFSET);

    // 轻微呼吸感动画：对齐 StartHappyFaceAnimation（33-38，900ms）
    const int NEUTRAL_EYE_ANIM_MIN = 33;
    const int NEUTRAL_EYE_ANIM_MAX = 38;
    const int NEUTRAL_EYE_ANIM_TIME = 900;

    // 创建动画数据，包含位置信息
    struct NeutralEyeAnimData {
        lv_obj_t* eye;
        int spacing;
        int y_offset;
        bool is_left;
    };
    
    // 使用静态变量保存动画数据，确保动画回调能访问到有效数据
    static NeutralEyeAnimData left_eye_data = {neutral_left_eye, NEUTRAL_EYE_SPACING, NEUTRAL_EYE_Y_OFFSET, true};
    static NeutralEyeAnimData right_eye_data = {neutral_right_eye, NEUTRAL_EYE_SPACING, NEUTRAL_EYE_Y_OFFSET, false};
    
    // 更新静态数据中的对象指针
    left_eye_data.eye = neutral_left_eye;
    left_eye_data.spacing = NEUTRAL_EYE_SPACING;
    left_eye_data.y_offset = NEUTRAL_EYE_Y_OFFSET;
    left_eye_data.is_left = true;
    
    right_eye_data.eye = neutral_right_eye;
    right_eye_data.spacing = NEUTRAL_EYE_SPACING;
    right_eye_data.y_offset = NEUTRAL_EYE_Y_OFFSET;
    right_eye_data.is_left = false;

    // 使用静态动画对象，避免动态分配问题
    static lv_anim_t left_eye_anim;
    static lv_anim_t right_eye_anim;
    
    // 先停止之前的动画，避免冲突
    lv_anim_del(&left_eye_anim, nullptr);
    lv_anim_del(&right_eye_anim, nullptr);
    
    lv_anim_init(&left_eye_anim);
    lv_anim_set_var(&left_eye_anim, &left_eye_data);
    lv_anim_set_values(&left_eye_anim, NEUTRAL_EYE_ANIM_MIN, NEUTRAL_EYE_ANIM_MAX);
    lv_anim_set_time(&left_eye_anim, NEUTRAL_EYE_ANIM_TIME);
    lv_anim_set_exec_cb(&left_eye_anim, [](void* data, int32_t v){
        NeutralEyeAnimData* anim_data = (NeutralEyeAnimData*)data;
        // 检查对象是否仍然有效
        if (anim_data->eye && lv_obj_is_valid(anim_data->eye)) {
            lv_obj_set_size(anim_data->eye, v, v);
            // 保持眼睛中心位置不变，向四周扩展
            if (anim_data->is_left) {
                lv_obj_align(anim_data->eye, LV_ALIGN_CENTER, -anim_data->spacing, anim_data->y_offset);
            } else {
                lv_obj_align(anim_data->eye, LV_ALIGN_CENTER, anim_data->spacing, anim_data->y_offset);
            }
        }
    });
    lv_anim_set_path_cb(&left_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim, NEUTRAL_EYE_ANIM_TIME);
    lv_anim_start(&left_eye_anim);

    lv_anim_init(&right_eye_anim);
    lv_anim_set_var(&right_eye_anim, &right_eye_data);
    lv_anim_set_values(&right_eye_anim, NEUTRAL_EYE_ANIM_MIN, NEUTRAL_EYE_ANIM_MAX);
    lv_anim_set_time(&right_eye_anim, NEUTRAL_EYE_ANIM_TIME);
    lv_anim_set_exec_cb(&right_eye_anim, [](void* data, int32_t v){
        NeutralEyeAnimData* anim_data = (NeutralEyeAnimData*)data;
        // 检查对象是否仍然有效
        if (anim_data->eye && lv_obj_is_valid(anim_data->eye)) {
            lv_obj_set_size(anim_data->eye, v, v);
            // 保持眼睛中心位置不变，向四周扩展
            if (anim_data->is_left) {
                lv_obj_align(anim_data->eye, LV_ALIGN_CENTER, -anim_data->spacing, anim_data->y_offset);
            } else {
                lv_obj_align(anim_data->eye, LV_ALIGN_CENTER, anim_data->spacing, anim_data->y_offset);
            }
        }
    });
    lv_anim_set_path_cb(&right_eye_anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim, NEUTRAL_EYE_ANIM_TIME);
    lv_anim_start(&right_eye_anim);

    // 保存容器指针，便于在切换表情时统一释放
    face_ = neutral_face;
}

