#include "eye_toy_single_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <vector>

#define TAG "EyeToySingleDisplay"

// Theme color structure
struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
};

// Define dark theme colors
static const ThemeColors DARK_THEME = {
    .background = lv_color_hex(0x121212),     // Dark background
    .text = lv_color_white(),                 // White text
    .chat_background = lv_color_hex(0x1E1E1E), // Slightly lighter than background
    .user_bubble = lv_color_hex(0x1A6C37),    // Dark green
    .assistant_bubble = lv_color_hex(0x333333), // Dark gray
    .system_bubble = lv_color_hex(0x2A2A2A),  // Medium gray
    .system_text = lv_color_hex(0xAAAAAA),    // Light gray text
    .border = lv_color_hex(0x333333),         // Dark gray border
    .low_battery = lv_color_hex(0xFF0000),    // Red for dark mode
};

// Define light theme colors
static const ThemeColors LIGHT_THEME = {
    .background = lv_color_white(),           // White background
    .text = lv_color_black(),                 // Black text
    .chat_background = lv_color_hex(0xE0E0E0), // Light gray background
    .user_bubble = lv_color_hex(0x95EC69),    // WeChat green
    .assistant_bubble = lv_color_white(),     // White
    .system_bubble = lv_color_hex(0xE0E0E0),  // Light gray
    .system_text = lv_color_hex(0x666666),    // Dark gray text
    .border = lv_color_hex(0xE0E0E0),         // Light gray border
    .low_battery = lv_color_black(),          // Black for light mode
};

// Current theme - initialize based on default config
static ThemeColors current_theme = LIGHT_THEME;
static std::string current_theme_name_ = "light";

// 单屏构造函数
EyeToySingleDisplay::EyeToySingleDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y,
                     DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), fonts_(fonts) {
    width_ = width;
    height_ = height;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;  // 提高LVGL任务优先级
    port_cfg.timer_period_ms = 60;  // 减少刷新周期
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 40),  // 增加缓冲区大小
        .double_buffer = true,  // 启用双缓冲
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

    // 给系统一些时间处理屏幕
    vTaskDelay(pdMS_TO_TICKS(100));

    // Update the theme
    if (current_theme_name_ == "dark") {
        current_theme = DARK_THEME;
    } else if (current_theme_name_ == "light") {
        current_theme = LIGHT_THEME;
    }

    SetupUI();
}

EyeToySingleDisplay::~EyeToySingleDisplay() {
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
    if (lcd_mutex_ != nullptr) {
        vSemaphoreDelete(lcd_mutex_);
    }
}

bool EyeToySingleDisplay::Lock(int timeout_ms) {
    if (lcd_mutex_ == nullptr) {
        lcd_mutex_ = xSemaphoreCreateMutex();
        if (lcd_mutex_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create LCD mutex");
            return false;
        }
    }
    return xSemaphoreTake(lcd_mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void EyeToySingleDisplay::Unlock() {
    if (lcd_mutex_ != nullptr) {
        xSemaphoreGive(lcd_mutex_);
    }
}

void EyeToySingleDisplay::SetEmotion(const char* emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emotion == nullptr) {
        return;
    }

    EyeState new_state = current_state_;
    
    if (strcmp(emotion, "neutral") == 0) {
        new_state = EyeState::IDLE;
    } else if (strcmp(emotion, "thinking") == 0) {
        new_state = EyeState::THINKING;
    } else if (strcmp(emotion, "relaxed") == 0) {
        new_state = EyeState::IDLE;
    } else if (strcmp(emotion, "sleepy") == 0) {
        new_state = EyeState::SLEEPING;
    }

    if (new_state == current_state_) {
        return;
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
        case EyeState::THINKING:
            ESP_LOGI(TAG, "StartThinkingAnimation");
            StartThinkingAnimation();
            break;
        case EyeState::LISTENING:
            StartIdleAnimation();
            break;
        case EyeState::SLEEPING:
            StartHalfClosedAnimation();
            break;
    }

    // 启动眨眼动画
    if (blink_mask_ && (current_state_ == EyeState::IDLE || current_state_ == EyeState::THINKING || current_state_ == EyeState::LISTENING)) {
        StartBlinkAnimation(blink_mask_);
    }
}

void EyeToySingleDisplay::SetIcon(const char* icon) {
    // Not implemented for eye display
}

void EyeToySingleDisplay::SetPreviewImage(const lv_img_dsc_t* img_dsc) {
    // Not implemented for eye display
}

void EyeToySingleDisplay::SetTheme(const std::string& theme_name) {
    current_theme_name_ = theme_name;
    if (theme_name == "dark") {
        current_theme = DARK_THEME;
    } else if (theme_name == "light") {
        current_theme = LIGHT_THEME;
    }
}

void EyeToySingleDisplay::StartIdleAnimation() {
    // 停止高光动画
    if (left_highlight_) {
        lv_anim_del(left_highlight_, nullptr);
        lv_obj_clear_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_highlight_) {
        lv_anim_del(right_highlight_, nullptr);
        lv_obj_clear_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 停止星星的动画并删除对象
    if (star_big_) {
        lv_anim_del(star_big_, nullptr);
        lv_obj_delete_delayed(star_big_, 0);
        star_big_ = nullptr;
    }
    if (star_small_) {
        lv_anim_del(star_small_, nullptr);
        lv_obj_delete_delayed(star_small_, 0);
        star_small_ = nullptr;
    }
}

void EyeToySingleDisplay::StartThinkingAnimation() {
    // 停止高光动画并隐藏高光
    if (left_highlight_) {
        lv_anim_del(left_highlight_, nullptr);
        lv_obj_add_flag(left_highlight_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_highlight_) {
        lv_anim_del(right_highlight_, nullptr);
        lv_obj_add_flag(right_highlight_, LV_OBJ_FLAG_HIDDEN);
    }

    // 创建大星星和小星星
    if (!star_big_) {
        star_big_ = lv_img_create(lv_obj_get_parent(left_highlight_));
        if (star_big_) {
            lv_img_set_src(star_big_, &star_img_32);
            // 使用缩放而不是固定尺寸
            lv_img_set_zoom(star_big_, 256);  // 256 = 100% zoom
            lv_obj_set_pos(star_big_, lv_obj_get_x(left_highlight_), lv_obj_get_y(left_highlight_));
            lv_obj_set_style_img_recolor(star_big_, lv_color_white(), 0);
            lv_obj_set_style_img_recolor_opa(star_big_, LV_OPA_COVER, 0);
            // 使用叠加混合模式
            lv_obj_set_style_blend_mode(star_big_, LV_BLEND_MODE_ADDITIVE, 0);
        }
    }

    if (!star_small_) {
        star_small_ = lv_img_create(lv_obj_get_parent(right_highlight_));
        if (star_small_) {
            lv_img_set_src(star_small_, &star_img_16);
            // 使用缩放而不是固定尺寸
            lv_img_set_zoom(star_small_, 256);  // 256 = 100% zoom
            lv_obj_set_pos(star_small_, lv_obj_get_x(right_highlight_), lv_obj_get_y(right_highlight_));
            lv_obj_set_style_img_recolor(star_small_, lv_color_white(), 0);
            lv_obj_set_style_img_recolor_opa(star_small_, LV_OPA_COVER, 0);
            // 使用叠加混合模式
            lv_obj_set_style_blend_mode(star_small_, LV_BLEND_MODE_ADDITIVE, 0);
        }
    }

    // 启动星星动画
    if (star_big_ && star_small_) {
        lv_anim_t anim1;
        lv_anim_init(&anim1);
        lv_anim_set_var(&anim1, star_big_);
        lv_anim_set_values(&anim1, 0, 3600);
        lv_anim_set_time(&anim1, 5000);
        lv_anim_set_delay(&anim1, 0);
        lv_anim_set_exec_cb(&anim1, (lv_anim_exec_xcb_t)lv_img_set_angle);
        lv_anim_set_path_cb(&anim1, lv_anim_path_linear);
        lv_anim_set_repeat_count(&anim1, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim1);

        lv_anim_init(&anim1);
        lv_anim_set_var(&anim1, star_small_);
        lv_anim_set_values(&anim1, 0, 3600);
        lv_anim_set_time(&anim1, 5000);
        lv_anim_set_delay(&anim1, 0);
        lv_anim_set_exec_cb(&anim1, (lv_anim_exec_xcb_t)lv_img_set_angle);
        lv_anim_set_path_cb(&anim1, lv_anim_path_linear);
        lv_anim_set_repeat_count(&anim1, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&anim1);
    }
}

void EyeToySingleDisplay::StartListeningAnimation() {
    StartIdleAnimation();
}

void EyeToySingleDisplay::StartSleepingAnimation() {
    StartIdleAnimation();
}

void EyeToySingleDisplay::StartHalfClosedAnimation() {
    ESP_LOGI(TAG, "StartHalfClosedAnimation");
    if (!blink_mask_) return;
    lv_anim_del(blink_mask_, nullptr); // 停止任何动画
    lv_obj_set_y(blink_mask_, -height_ / 2); // 遮住一半
}

void EyeToySingleDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    // 设置屏幕
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, current_theme.background, 0);
    // 禁用屏幕滚动与滚动条
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // 创建眼睛
    CreateEye(screen);
}

// 创建眼睛的辅助函数
void EyeToySingleDisplay::CreateEye(lv_obj_t* parent) {
    // 1. 外圈（棕色），填满屏幕，但留出边距
    int horizontal_padding = width_ * 0.2;   // 水平方向留出 20% 的边距
    int vertical_padding = height_ * 0.17;    // 垂直方向留出 17% 的边距

    lv_obj_t* eye_outer = lv_obj_create(parent);
    lv_obj_set_size(eye_outer, width_ + 4, height_ + 4);
    lv_obj_set_style_radius(eye_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye_outer, lv_color_hex(0x6B5B27), 0); // 棕色
    lv_obj_set_style_border_width(eye_outer, 0, 0);
    lv_obj_set_style_pad_all(eye_outer, 0, 0);
    lv_obj_center(eye_outer);
    // 细调整体垂直偏移，向上轻微移动以消除顶部可见边缘
    lv_obj_set_y(eye_outer, lv_obj_get_y(eye_outer) - 2);
    // 禁用外圈滚动与滚动条
    lv_obj_clear_flag(eye_outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(eye_outer, LV_SCROLLBAR_MODE_OFF);

    // 2. 眼球（纯黑色），比外圈小一些，保持椭圆形
    int ball_w = (width_ - 2 * horizontal_padding) * 0.6;
    int ball_h = (height_ - 2 * vertical_padding) * 0.68;  // 高度稍微小一些，使眼睛更椭圆
    lv_obj_t* eye_ball = lv_obj_create(eye_outer);
    lv_obj_set_size(eye_ball, ball_w, ball_h);
    lv_obj_set_style_radius(eye_ball, LV_RADIUS_CIRCLE, 0);  // 使用最大圆角
    lv_obj_set_style_bg_color(eye_ball, lv_color_black(), 0); // 纯黑色
    lv_obj_set_style_border_width(eye_ball, 0, 0);
    lv_obj_set_style_pad_all(eye_ball, 0, 0);
    lv_obj_center(eye_ball);
    lv_obj_set_y(eye_ball, lv_obj_get_y(eye_ball) - 20); // 整体上移 20 像素
    // 禁用眼球滚动与滚动条
    lv_obj_clear_flag(eye_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(eye_ball, LV_SCROLLBAR_MODE_OFF);

    // 3. 高光1（大白点）
    lv_obj_t* highlight1 = lv_obj_create(eye_ball);
    lv_obj_set_size(highlight1, ball_w * 0.24, ball_h * 0.25);
    lv_obj_set_style_radius(highlight1, ball_w * 0.125, 0);  // 使用高度的一半作为圆角
    lv_obj_set_style_bg_color(highlight1, lv_color_white(), 0);
    lv_obj_set_style_border_width(highlight1, 0, 0);
    lv_obj_set_style_pad_all(highlight1, 0, 0);
    lv_obj_set_style_bg_opa(highlight1, LV_OPA_COVER, 0);
    // 向下微调高光，避免与顶部黄色边缘产生对比
    lv_obj_set_pos(highlight1, ball_w * 0.10, ball_h * 0.12);
    lv_obj_clear_flag(highlight1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(highlight1, LV_SCROLLBAR_MODE_OFF);

    // 4. 高光2（小白点）
    lv_obj_t* highlight2 = lv_obj_create(eye_ball);
    lv_obj_set_size(highlight2, ball_w * 0.12, ball_h * 0.13);
    lv_obj_set_style_radius(highlight2, ball_w * 0.065, 0);  // 使用高度的一半作为圆角
    lv_obj_set_style_bg_color(highlight2, lv_color_white(), 0);
    lv_obj_set_style_border_width(highlight2, 0, 0);
    lv_obj_set_style_pad_all(highlight2, 0, 0);
    lv_obj_set_style_bg_opa(highlight2, LV_OPA_COVER, 0);
    lv_obj_set_pos(highlight2, ball_w * 0.45, ball_h * 0.24);
    lv_obj_clear_flag(highlight2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(highlight2, LV_SCROLLBAR_MODE_OFF);

    // 5. 创建眨眼遮罩层
    lv_obj_t* blink_mask = lv_obj_create(eye_outer);
    lv_obj_set_size(blink_mask, width_, height_);
    lv_obj_set_style_radius(blink_mask, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(blink_mask, lv_color_black(), 0);
    lv_obj_set_style_border_width(blink_mask, 0, 0);
    lv_obj_set_style_pad_all(blink_mask, 0, 0);
    lv_obj_set_pos(blink_mask, 0, -height_);  // 初始位置在眼睛上方
    lv_obj_set_style_bg_opa(blink_mask, LV_OPA_COVER, 0);
    lv_obj_clear_flag(blink_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(blink_mask, LV_SCROLLBAR_MODE_OFF);

    // 保存高光对象供状态切换使用
    left_highlight_ = highlight1;
    right_highlight_ = highlight2;
    left_eye_ = highlight1;
    right_eye_ = highlight2;
    blink_mask_ = blink_mask;
}

void EyeToySingleDisplay::StartBlinkAnimation(lv_obj_t* blink_mask) {
    // 创建眨眼动画
    lv_anim_t blink_anim;
    lv_anim_init(&blink_anim);
    lv_anim_set_var(&blink_anim, blink_mask);
    lv_anim_set_values(&blink_anim, -height_, 0);  // 从上到下移动
    lv_anim_set_time(&blink_anim, 150);       // 眨眼动作持续150ms
    lv_anim_set_delay(&blink_anim, 0);
    lv_anim_set_exec_cb(&blink_anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&blink_anim, lv_anim_path_linear);  // 使用线性动画路径
    lv_anim_set_repeat_count(&blink_anim, LV_ANIM_REPEAT_INFINITE);  // 无限循环
    lv_anim_set_playback_time(&blink_anim, 150);  // 恢复动作持续150ms
    lv_anim_set_playback_delay(&blink_anim, 0);
    lv_anim_set_repeat_delay(&blink_anim, 5000);  // 每5秒重复一次
    lv_anim_start(&blink_anim);
} 