#include "eye_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>

#define TAG "EyeDisplay"

EyeDisplay::EyeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y,
                     DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), fonts_(fonts) {
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
    port_cfg.timer_period_ms = 50;
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
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
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
    EyeState new_state = current_state_;
    
    if (strcmp(emotion, "neutral") == 0) {
        new_state = EyeState::IDLE;
    } else if (strcmp(emotion, "happy") == 0) {
        new_state = EyeState::HAPPY;
    } else if (strcmp(emotion, "laughing") == 0) {
        new_state = EyeState::LAUGHING;
    } else if (strcmp(emotion, "sad") == 0) {
        new_state = EyeState::SAD;
    } else if (strcmp(emotion, "angry") == 0) {
        new_state = EyeState::ANGRY;
    } else if (strcmp(emotion, "crying") == 0) {
        new_state = EyeState::CRYING;
    } else if (strcmp(emotion, "loving") == 0) {
        new_state = EyeState::LOVING;
    } else if (strcmp(emotion, "embarrassed") == 0) {
        new_state = EyeState::EMBARRASSED;
    } else if (strcmp(emotion, "surprised") == 0) {
        new_state = EyeState::SURPRISED;
    } else if (strcmp(emotion, "shocked") == 0) {
        new_state = EyeState::SHOCKED;
    } else if (strcmp(emotion, "thinking") == 0) {
        new_state = EyeState::THINKING;
    } else if (strcmp(emotion, "winking") == 0) {
        new_state = EyeState::WINKING;
    } else if (strcmp(emotion, "cool") == 0) {
        new_state = EyeState::COOL;
    } else if (strcmp(emotion, "relaxed") == 0) {
        new_state = EyeState::RELAXED;
    } else if (strcmp(emotion, "delicious") == 0) {
        new_state = EyeState::DELICIOUS;
    } else if (strcmp(emotion, "kissy") == 0) {
        new_state = EyeState::KISSY;
    } else if (strcmp(emotion, "confident") == 0) {
        new_state = EyeState::CONFIDENT;
    } else if (strcmp(emotion, "sleepy") == 0) {
        new_state = EyeState::SLEEPING;
    } else if (strcmp(emotion, "silly") == 0) {
        new_state = EyeState::SILLY;
    } else if (strcmp(emotion, "confused") == 0) {
        new_state = EyeState::CONFUSED;
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

    current_state_ = new_state;

    // 停止当前动画
    lv_anim_del(left_eye_, nullptr);
    lv_anim_del(right_eye_, nullptr);

    // 根据状态启动新的动画
    switch (current_state_) {
        case EyeState::IDLE:
            StartIdleAnimation();
            break;
        case EyeState::HAPPY:
            StartHappyAnimation();
            break;
        case EyeState::LAUGHING:
            StartLaughingAnimation();
            break;
        case EyeState::SAD:
            StartSadAnimation();
            break;
        case EyeState::ANGRY:
            StartAngryAnimation();
            break;
        case EyeState::CRYING:
            StartCryingAnimation();
            break;
        case EyeState::LOVING:
            StartLovingAnimation();
            break;
        case EyeState::EMBARRASSED:
            StartEmbarrassedAnimation();
            break;
        case EyeState::SURPRISED:
            StartSurprisedAnimation();
            break;
        case EyeState::SHOCKED:
            StartShockedAnimation();
            break;
        case EyeState::THINKING:
            StartThinkingAnimation();
            break;
        case EyeState::WINKING:
            StartWinkingAnimation();
            break;
        case EyeState::COOL:
            StartCoolAnimation();
            break;
        case EyeState::RELAXED:
            StartRelaxedAnimation();
            break;
        case EyeState::DELICIOUS:
            StartDeliciousAnimation();
            break;
        case EyeState::KISSY:
            StartKissyAnimation();
            break;
        case EyeState::CONFIDENT:
            StartConfidentAnimation();
            break;
        case EyeState::SLEEPING:
            StartSleepingAnimation();
            break;
        case EyeState::SILLY:
            StartSillyAnimation();
            break;
        case EyeState::CONFUSED:
            StartConfusedAnimation();
            break;
    }
}

void EyeDisplay::StartIdleAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
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
    lv_anim_set_values(&right_anim_, 40, 80);
    lv_anim_set_time(&right_anim_, 2000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 2000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartHappyAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 60, 70);
    lv_anim_set_time(&left_anim_, 1000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 60, 70);
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartLaughingAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 80);
    lv_anim_set_time(&left_anim_, 300);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 300);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 30, 80);
    lv_anim_set_time(&right_anim_, 300);
    lv_anim_set_delay(&right_anim_, 150);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 300);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartSadAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 50, 60);
    lv_anim_set_time(&left_anim_, 2000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 2000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 50, 60);
    lv_anim_set_time(&right_anim_, 2000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 2000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartAngryAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 20, 30);
    lv_anim_set_time(&left_anim_, 500);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 500);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 20, 30);
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_delay(&right_anim_, 250);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartCryingAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 20, 70);
    lv_anim_set_time(&left_anim_, 200);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 200);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 20, 70);
    lv_anim_set_time(&right_anim_, 200);
    lv_anim_set_delay(&right_anim_, 100);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 200);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartLovingAnimation() {
    lv_obj_set_style_radius(left_eye_, 40, 0);
    lv_obj_set_style_radius(right_eye_, 40, 0);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 60, 70);
    lv_anim_set_time(&left_anim_, 1000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 60, 70);
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 500);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartSurprisedAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 90);
    lv_anim_set_time(&left_anim_, 200);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 200);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 90);
    lv_anim_set_time(&right_anim_, 200);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 200);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartThinkingAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 50, 70);
    lv_anim_set_time(&left_anim_, 1500);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1500);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 50, 70);
    lv_anim_set_time(&right_anim_, 1500);
    lv_anim_set_delay(&right_anim_, 750);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1500);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartWinkingAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 80);
    lv_anim_set_time(&left_anim_, 2000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 2000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    // 右眼保持静态
    lv_obj_set_size(right_eye_, 40, 80);
}

void EyeDisplay::StartCoolAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 40);
    lv_anim_set_time(&left_anim_, 1000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 30, 40);
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 500);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartRelaxedAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 50, 60);
    lv_anim_set_time(&left_anim_, 2000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 2000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 50, 60);
    lv_anim_set_time(&right_anim_, 2000);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 2000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartSleepingAnimation() {
    // 设置眼睛为水平长条
    lv_obj_set_size(left_eye_, 80, 20);
    lv_obj_set_size(right_eye_, 80, 20);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);

    // 创建三个 z 标签
    zzz1_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz1_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz1_, lv_color_make(0, 255, 255), 0);  // 黄色
    lv_label_set_text(zzz1_, "z");
    lv_obj_align(zzz1_, LV_ALIGN_TOP_MID, -40, 50);  // 调整垂直位置到 50
    lv_obj_set_style_text_letter_space(zzz1_, 2, 0);  // 增加字间距

    zzz2_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz2_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz2_, lv_color_make(0, 255, 255), 0);  // 黄色
    lv_label_set_text(zzz2_, "z");
    lv_obj_align(zzz2_, LV_ALIGN_TOP_MID, 0, 40);  // 调整垂直位置到 40
    lv_obj_set_style_text_letter_space(zzz2_, 2, 0);  // 增加字间距

    zzz3_ = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(zzz3_, fonts_.text_font, 0);  // 使用文本字体
    lv_obj_set_style_text_color(zzz3_, lv_color_make(0, 255, 255), 0);  // 黄色
    lv_label_set_text(zzz3_, "z");
    lv_obj_align(zzz3_, LV_ALIGN_TOP_MID, 40, 30);  // 调整垂直位置到 30
    lv_obj_set_style_text_letter_space(zzz3_, 2, 0);  // 增加字间距
}

void EyeDisplay::StartConfusedAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 70);
    lv_anim_set_time(&left_anim_, 800);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 800);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 70);
    lv_anim_set_time(&right_anim_, 800);
    lv_anim_set_delay(&right_anim_, 400);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 800);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartEmbarrassedAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 45, 55);
    lv_anim_set_time(&left_anim_, 300);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 300);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 45, 55);
    lv_anim_set_time(&right_anim_, 300);
    lv_anim_set_delay(&right_anim_, 150);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 300);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartShockedAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 40, 100);
    lv_anim_set_time(&left_anim_, 100);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 100);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 40, 100);
    lv_anim_set_time(&right_anim_, 100);
    lv_anim_set_delay(&right_anim_, 0);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_overshoot);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 100);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartDeliciousAnimation() {
    lv_obj_set_style_radius(left_eye_, 40, 0);
    lv_obj_set_style_radius(right_eye_, 40, 0);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 50, 70);
    lv_anim_set_time(&left_anim_, 500);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 500);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 50, 70);
    lv_anim_set_time(&right_anim_, 500);
    lv_anim_set_delay(&right_anim_, 250);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 500);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartKissyAnimation() {
    lv_obj_set_style_radius(left_eye_, 40, 0);
    lv_obj_set_style_radius(right_eye_, 40, 0);
    
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 55, 65);
    lv_anim_set_time(&left_anim_, 1000);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1000);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 55, 65);
    lv_anim_set_time(&right_anim_, 1000);
    lv_anim_set_delay(&right_anim_, 500);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1000);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartConfidentAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 35, 45);
    lv_anim_set_time(&left_anim_, 1500);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 1500);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 35, 45);
    lv_anim_set_time(&right_anim_, 1500);
    lv_anim_set_delay(&right_anim_, 750);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 1500);
    lv_anim_set_playback_delay(&right_anim_, 0);
    lv_anim_start(&right_anim_);
}

void EyeDisplay::StartSillyAnimation() {
    lv_anim_init(&left_anim_);
    lv_anim_set_var(&left_anim_, left_eye_);
    lv_anim_set_values(&left_anim_, 30, 90);
    lv_anim_set_time(&left_anim_, 400);
    lv_anim_set_delay(&left_anim_, 0);
    lv_anim_set_exec_cb(&left_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&left_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_anim_, 400);
    lv_anim_set_playback_delay(&left_anim_, 0);
    lv_anim_start(&left_anim_);

    lv_anim_init(&right_anim_);
    lv_anim_set_var(&right_anim_, right_eye_);
    lv_anim_set_values(&right_anim_, 30, 90);
    lv_anim_set_time(&right_anim_, 400);
    lv_anim_set_delay(&right_anim_, 200);
    lv_anim_set_exec_cb(&right_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_anim_, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&right_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_anim_, 400);
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
    lv_obj_set_style_bg_color(left_eye_, lv_color_make(0, 255, 255), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_eye_, 0, 0);
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);
    lv_obj_set_style_outline_width(left_eye_, 0, 0);

    // Create right eye
    right_eye_ = lv_obj_create(container);
    lv_obj_set_size(right_eye_, 40, 80);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_make(0, 255, 255), 0);  // BGR: 黄色
    lv_obj_set_style_border_width(right_eye_, 0, 0);
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_eye_, 0, 0);
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);
    lv_obj_set_style_outline_width(right_eye_, 0, 0);

    // 启动默认的待机动画
    StartIdleAnimation();
} 