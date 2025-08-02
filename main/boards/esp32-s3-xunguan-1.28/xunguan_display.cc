#include "xunguan_display.h"
#include "config.h"
#include "assert.h"
#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>
#include <cstring>
#include <lvgl.h>
#include "application.h"

/*
 * 帧率控制接口使用示例：
 * 
 * // 1. 设置预定义模式
 * display->SetFrameRateMode(XunguanDisplay::FrameRateMode::POWER_SAVE);   // 低功耗模式
 * display->SetFrameRateMode(XunguanDisplay::FrameRateMode::NORMAL);       // 正常模式
 * display->SetFrameRateMode(XunguanDisplay::FrameRateMode::SMOOTH);       // 流畅模式
 * 
 * // 2. 设置自定义帧率
 * display->SetCustomFrameRate(10, 25);  // 10-25ms延迟范围
 * 
 * // 3. 获取当前设置
 * auto mode = display->GetCurrentFrameRateMode();
 * auto min_delay = display->GetCurrentMinDelay();
 * auto max_delay = display->GetCurrentMaxDelay();
 * auto tick_period = display->GetCurrentTickPeriod();
 * 
 * 帧率模式说明：
 * - POWER_SAVE: 8-20ms延迟，8ms tick (50-125Hz) - 最低功耗
 * - NORMAL: 5-15ms延迟，2ms tick (67-200Hz) - 平衡性能和功耗
 * - SMOOTH: 2-8ms延迟，1ms tick (125-500Hz) - 最高流畅度
 * - CUSTOM: 自定义延迟范围
 * 
 * 动画函数使用示例：
 * // 文本透明度动画 - 改变文本的透明度（0-255），实现淡入淡出效果
 * // 适用于文本标签、按钮等有文本内容的对象
 * display->StartSimpleColorAnimation(some_text_label);
 * 
 * // 睡眠UI - 使用绝对布局创建三个"z"标签，实现睡眠动画效果
 * display->StartSleepingAnimation();
 */

#define TAG "XunguanDisplay"
#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

// Static member initialization
_lock_t XunguanDisplay::lvgl_api_lock = {0};

XunguanDisplay::XunguanDisplay() 
    : Display(), pending_animation_(), animation_queue_enabled_(true),
      panel_io_(nullptr), panel_(nullptr), lvgl_display_(nullptr), initialized_(false),
      current_frame_rate_mode_(FrameRateMode::NORMAL),
      current_min_delay_ms_(5), current_max_delay_ms_(15), current_tick_period_us_(2000),
      left_eye_(nullptr), container_(nullptr), 
      zzz1_(nullptr), zzz2_(nullptr), zzz3_(nullptr),
      zzz1_anim_(), zzz2_anim_(), zzz3_anim_(),
      current_state_(EyeState::IDLE),
      left_eye_anim_(), right_eye_anim_(), right_eye_(nullptr),
      mouth_(nullptr), mouth_anim_(),
      lvgl_tick_timer_(nullptr), lvgl_task_handle_(nullptr),
      vertigo_recovery_timer_(nullptr), vertigo_mode_active_(false),
      loving_mode_active_(false), ota_progress_bar_(nullptr), ota_number_label_(nullptr), ota_progress_(0) {
    
    // Initialize static lock
    _lock_init(&lvgl_api_lock);
}

XunguanDisplay::~XunguanDisplay() {
    if (lvgl_tick_timer_) {
        esp_timer_stop(lvgl_tick_timer_);
        esp_timer_delete(lvgl_tick_timer_);
    }
    
    if (vertigo_recovery_timer_) {
        vertigo_mode_active_ = false;
        esp_timer_stop(vertigo_recovery_timer_);
        esp_timer_delete(vertigo_recovery_timer_);
    }
    
    if (loving_recovery_timer_) {
        loving_mode_active_ = false;
        esp_timer_stop(loving_recovery_timer_);
        esp_timer_delete(loving_recovery_timer_);
    }
    
    if (lvgl_task_handle_) {
        vTaskDelete(lvgl_task_handle_);
    }
    
    if (lvgl_display_) {
        lv_display_delete(lvgl_display_);
    }
}

bool XunguanDisplay::Initialize() {
    
    
    
    if (!InitializeSpi()) {
        ESP_LOGE(TAG, "Failed to initialize SPI");
        return false;
    }
    
    
    
    if (!InitializeLcdPanel()) {
        ESP_LOGE(TAG, "Failed to initialize LCD panel");
        return false;
    }
    
    
    
    if (!InitializeLvgl()) {
        ESP_LOGE(TAG, "Failed to initialize LVGL");
        return false;
    }
    
    
    
    if (!InitializeLvglTimer()) {
        ESP_LOGE(TAG, "Failed to initialize LVGL timer");
        return false;
    }
    
    
    
    if (!CreateLvglTask()) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return false;
    }
    
    
    
    // Setup UI after everything is initialized
    SetupUI();
    
    
    // Force a manual refresh immediately
    
    lv_refr_now(lvgl_display_);
    
    // Start idle animation after initialization
    
    StartIdleAnimation();
    
    // Add a small delay to ensure everything is ready
    vTaskDelay(pdMS_TO_TICKS(100));
    
    initialized_ = true;
    
    return true;
}

bool XunguanDisplay::InitializeSpi() {
    
    
    spi_bus_config_t buscfg = {
        .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
        .miso_io_num = -1,  // No MISO for this display
        .sclk_io_num = DISPLAY_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 80 * sizeof(uint16_t),
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool XunguanDisplay::InitializeLcdPanel() {
    
    
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_SPI_CS_PIN,
        .dc_gpio_num = DISPLAY_SPI_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_SCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &panel_io_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO creation failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    
    ret = esp_lcd_new_panel_gc9a01(panel_io_, &panel_config, &panel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_lcd_panel_reset(panel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_lcd_panel_init(panel_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Invert colors for GC9A01
    ret = esp_lcd_panel_invert_color(panel_, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel color invert failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Mirror display
    ret = esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel mirror failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Turn on display
    ret = esp_lcd_panel_disp_on_off(panel_, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel display on failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool XunguanDisplay::InitializeLvgl() {
    
    
    lv_init();
    
    // Create LVGL display
    lvgl_display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!lvgl_display_) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return false;
    }
    
    
    
    // Enable anti-aliasing and improve rendering quality
    lv_display_set_antialiasing(lvgl_display_, true);
    lv_display_set_dpi(lvgl_display_, 160);
    
    // Allocate draw buffers - make them larger for better performance
    size_t draw_buffer_sz = DISPLAY_WIDTH * 40 * sizeof(lv_color16_t);
    void* buf1 = heap_caps_malloc(draw_buffer_sz, MALLOC_CAP_DMA);
    void* buf2 = heap_caps_malloc(draw_buffer_sz, MALLOC_CAP_DMA);
    
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate draw buffers");
        return false;
    }
    
    // Initialize LVGL draw buffers
    lv_display_set_buffers(lvgl_display_, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    
    // Associate panel handle to display
    lv_display_set_user_data(lvgl_display_, panel_);
    
    // Set color format
    lv_display_set_color_format(lvgl_display_, LV_COLOR_FORMAT_RGB565);
    
    // Set flush callback
    lv_display_set_flush_cb(lvgl_display_, lvgl_flush_cb);
    
    // Set rotation if needed
    lv_display_set_rotation(lvgl_display_, LV_DISPLAY_ROTATION_0);
    
    return true;
}

bool XunguanDisplay::InitializeLvglTimer() {
    
    
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = [](void* arg) {
            lv_tick_inc(2);  // 2ms tick
        },
        .name = "lvgl_tick"
    };
    
    esp_err_t ret = esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LVGL tick timer: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 使用当前配置的tick周期
    ret = esp_timer_start_periodic(lvgl_tick_timer_, current_tick_period_us_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer: %s", esp_err_to_name(ret));
        return false;
    }
    
    
    return true;
}

bool XunguanDisplay::CreateLvglTask() {
    
    
    BaseType_t ret = xTaskCreate(
        lvgl_task,
        "LVGL",
        8192,  // Increase stack size
        this,
        5,     // Increase priority
        &lvgl_task_handle_
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return false;
    }
    
    
    return true;
}

void XunguanDisplay::lvgl_task(void* arg) {
    XunguanDisplay* self = static_cast<XunguanDisplay*>(arg);
    
    
    uint32_t time_till_next_ms = 0;
    
    // Log immediately to confirm task started
    
    
    while (1) {
        
        // Minimize lock holding time
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        
        // 使用当前配置的延迟范围
        time_till_next_ms = MAX(time_till_next_ms, self->current_min_delay_ms_);
        time_till_next_ms = MIN(time_till_next_ms, self->current_max_delay_ms_);
        vTaskDelay(pdMS_TO_TICKS(time_till_next_ms));
        
    }
}

void XunguanDisplay::lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    
    // Swap RGB bytes for SPI LCD
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    
    // Copy buffer to display
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel draw bitmap failed: %s", esp_err_to_name(ret));
    } else {
    }
    
    // Notify LVGL that flush is complete
    lv_display_flush_ready(disp);
}

void XunguanDisplay::lvgl_port_update_callback(lv_display_t* disp) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    
    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISPLAY_ROTATION_90:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISPLAY_ROTATION_180:
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISPLAY_ROTATION_270:
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}

bool XunguanDisplay::Lock(int timeout_ms) {
    // Simple lock acquisition
    _lock_acquire(&lvgl_api_lock);
    return true;
}

void XunguanDisplay::Unlock() {
    _lock_release(&lvgl_api_lock);
}

void XunguanDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    
    
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    // Force a manual refresh
    lv_refr_now(lvgl_display_);
    
    
}

void XunguanDisplay::SetEmotion(const char* emotion) {
    
    
    if (!animation_queue_enabled_) {
        ESP_LOGW(TAG, "Animation queue is disabled");
        return;
    }
    
    // Check if vertigo mode is active - disable emotion changes during vertigo
    if (vertigo_mode_active_) {
        ESP_LOGW(TAG, "Vertigo mode active, ignoring emotion change: %s", emotion);
        return;
    }
    
    // Check if loving animation is active - disable emotion changes during loving animation
    if (loving_mode_active_) {
        ESP_LOGW(TAG, "Loving mode active, ignoring emotion change: %s", emotion);
        return;
    }
    
    // Map emotion string to animation type
    AnimationType anim_type = AnimationType::NONE;
    
    if (strcmp(emotion, "neutral") == 0) {
        anim_type = AnimationType::IDLE;
    } else if (strcmp(emotion, "happy") == 0) {
        anim_type = AnimationType::HAPPY;
    } else if (strcmp(emotion, "laughing") == 0) {
        anim_type = AnimationType::LOVING;
    } else if (strcmp(emotion, "sad") == 0) {
        anim_type = AnimationType::SAD;
    } else if (strcmp(emotion, "angry") == 0) {
        anim_type = AnimationType::SAD;
    } else if (strcmp(emotion, "crying") == 0) {
        anim_type = AnimationType::SAD;
    } else if (strcmp(emotion, "loving") == 0) {
        anim_type = AnimationType::LOVING;
    } else if (strcmp(emotion, "embarrassed") == 0) {
        anim_type = AnimationType::SHOCKED;
    } else if (strcmp(emotion, "surprised") == 0) {
        anim_type = AnimationType::SHOCKED;
    } else if (strcmp(emotion, "shocked") == 0) {
        anim_type = AnimationType::SHOCKED;
    } else if (strcmp(emotion, "thinking") == 0) {
        anim_type = AnimationType::THINKING;
    } else if (strcmp(emotion, "winking") == 0) {
        anim_type = AnimationType::HAPPY;
    } else if (strcmp(emotion, "cool") == 0) {
        anim_type = AnimationType::HAPPY;
    } else if (strcmp(emotion, "relaxed") == 0) {
        anim_type = AnimationType::HAPPY;
    } else if (strcmp(emotion, "delicious") == 0) {
        anim_type = AnimationType::SHOCKED;
    } else if (strcmp(emotion, "kissy") == 0) {
        anim_type = AnimationType::LOVING;
    } else if (strcmp(emotion, "confident") == 0) {
        anim_type = AnimationType::HAPPY;
    } else if (strcmp(emotion, "sleepy") == 0) {
        anim_type = AnimationType::SLEEPING;
    } else if (strcmp(emotion, "silly") == 0) {
        anim_type = AnimationType::SILLY;
    } else if (strcmp(emotion, "confused") == 0) {
        anim_type = AnimationType::SHOCKED;
    } else if (strcmp(emotion, "vertigo") == 0) {
        anim_type = AnimationType::VERTIGO;
    }
    
    if (anim_type == AnimationType::NONE) {
        ESP_LOGW(TAG, "Unknown emotion: %s", emotion);
        return;
    }
    
    // Queue the animation
    QueueAnimation(anim_type);
}

void XunguanDisplay::TestNextEmotion() {
    // Empty - no emotion testing needed
}

// Animation methods - empty implementations
void XunguanDisplay::StartIdleAnimation() {
    
    
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);

    
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Create left eye
    left_eye_ = lv_obj_create(screen);
    
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left eye!");
        return;
    }
    
    // Calculate eye dimensions and positions based on screen size
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    // Eye dimensions - Y axis longer than X axis
    int eye_width = screen_width / 6;   // 1/6 of screen width
    int eye_height = eye_width * 2;     // Y axis twice as long as X axis
    
    // Calculate positions for centered eyes
    int eye_spacing = screen_width / 3;  // 1/3 of screen width between eyes (increased from 1/4)
    int left_eye_x = (screen_width / 2) - (eye_spacing / 2) - (eye_width / 2);
    int right_eye_x = (screen_width / 2) + (eye_spacing / 2) - (eye_width / 2);
    int eye_y = (screen_height / 2) - (eye_height / 2);
    
    int y_offset = 2;
    // Set left eye properties
    lv_obj_set_size(left_eye_, eye_width, eye_height);
    lv_obj_set_pos(left_eye_, left_eye_x, eye_y - y_offset);
    lv_obj_set_style_radius(left_eye_, eye_width / 2, 0);  // Rounded corners
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);  // No border
    
    
    
    // Create right eye
    right_eye_ = lv_obj_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right eye!");
        return;
    }
    
    // Set right eye properties
    lv_obj_set_size(right_eye_, eye_width, eye_height);
    lv_obj_set_pos(right_eye_, right_eye_x, eye_y - y_offset);
    lv_obj_set_style_radius(right_eye_, eye_width / 2, 0);  // Rounded corners
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);  // No border
    
    
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    
    
    // Start Y-axis scaling animation for both eyes
    
    StartEyeScalingAnimation(left_eye_, right_eye_, eye_height);
}

void XunguanDisplay::StartHappyAnimation() {
    
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    StartIdleAnimation();
    DisplayLockGuard lock(this);
    
    // 获取屏幕尺寸
    int width_ = DISPLAY_WIDTH;
    int height_ = DISPLAY_HEIGHT;
    
    // 创建嘴巴图片对象
    mouth_ = lv_img_create(screen);
    lv_img_set_src(mouth_, &down_image);
    lv_obj_set_pos(mouth_, (width_ - 32) / 2, height_ - 52);  // 居中，距离底部52像素
    lv_obj_set_style_img_recolor(mouth_, lv_color_hex(EYE_COLOR), 0);  // 设置青色
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

void XunguanDisplay::StartSadAnimation() {
    
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Calculate screen dimensions
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    
    
    // Calculate positions for centered eyes
    int eye_spacing = screen_width / 3;  // 1/3 of screen width between eyes
    int left_eye_x = (screen_width / 2) - (eye_spacing / 2);
    int right_eye_x = (screen_width / 2) + (eye_spacing / 2);
    int eye_y = (screen_height / 2) - 10;  // Slightly above center
    
    // Create left eye (horizontal bar)
    left_eye_ = lv_obj_create(screen);
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left eye!");
        return;
    }
    
    // Set left eye properties - horizontal bar shape
    lv_obj_set_size(left_eye_, 60, 20);
    lv_obj_set_pos(left_eye_, left_eye_x - 30, eye_y - 10);
    lv_obj_set_style_radius(left_eye_, LV_RADIUS_CIRCLE, 0);  // Circular radius
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);  // No border
    
    
    
    // Create right eye (horizontal bar)
    right_eye_ = lv_obj_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right eye!");
        return;
    }
    
    // Set right eye properties - horizontal bar shape
    lv_obj_set_size(right_eye_, 60, 20);
    lv_obj_set_pos(right_eye_, right_eye_x - 30, eye_y - 10);
    lv_obj_set_style_radius(right_eye_, LV_RADIUS_CIRCLE, 0);  // Circular radius
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);  // No border
    
    
    
    // Create left tear (ellipse)
    lv_obj_t* left_tear = lv_obj_create(screen);
    if (!left_tear) {
        ESP_LOGE(TAG, "Failed to create left tear!");
        return;
    }
    
    // Set left tear properties - ellipse shape
    lv_obj_set_size(left_tear, 12, 20);  // Ellipse shape
    lv_obj_set_style_radius(left_tear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(left_tear, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_tear, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_tear, 0, 0);
    lv_obj_set_style_border_side(left_tear, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(left_tear, 0, 0);
    lv_obj_set_style_shadow_width(left_tear, 0, 0);
    lv_obj_set_style_outline_width(left_tear, 0, 0);
    
    // Position left tear below left eye
    lv_obj_set_pos(left_tear, left_eye_x - 6, eye_y + 20);
    
    
    
    // Create right tear (ellipse)
    lv_obj_t* right_tear = lv_obj_create(screen);
    if (!right_tear) {
        ESP_LOGE(TAG, "Failed to create right tear!");
        return;
    }
    
    // Set right tear properties - ellipse shape
    lv_obj_set_size(right_tear, 12, 20);  // Ellipse shape
    lv_obj_set_style_radius(right_tear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(right_tear, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_tear, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_tear, 0, 0);
    lv_obj_set_style_border_side(right_tear, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_pad_all(right_tear, 0, 0);
    lv_obj_set_style_shadow_width(right_tear, 0, 0);
    lv_obj_set_style_outline_width(right_tear, 0, 0);
    
    // Position right tear below right eye
    lv_obj_set_pos(right_tear, right_eye_x - 6, eye_y + 20);
    
    
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    
    
    // Start tear falling animations
    StartTearFallingAnimation(left_tear, right_tear, eye_y + 20);
}

void XunguanDisplay::StartLovingAnimation() {
    auto screen = lv_screen_active();
    if (!screen) {
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // 隐藏原来的眼睛（如果有的话）
    if (left_eye_) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_eye_) {
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 计算爱心位置
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    int heart_width = hart_img.header.w;
    int heart_height = hart_img.header.h;
    
    // 计算左右爱心的位置，确保有足够间隔
    int left_heart_x = 10;  // 距离左边缘20像素
    int right_heart_x = screen_width - 10 - heart_width;  // 距离右边缘20像素
    int heart_y = (screen_height / 2) - (heart_height / 2) - 20;  // 屏幕中心向上20像素
    
    // 创建左眼爱心图片
    left_eye_ = lv_img_create(screen);
    lv_img_set_src(left_eye_, &hart_img);
    lv_obj_set_style_img_recolor(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_size(left_eye_, heart_width, heart_height);
    lv_obj_set_pos(left_eye_, left_heart_x, heart_y);
    
    // 创建右眼爱心图片
    right_eye_ = lv_img_create(screen);
    lv_img_set_src(right_eye_, &hart_img);
    lv_obj_set_style_img_recolor(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_img_recolor_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_size(right_eye_, heart_width, heart_height);
    lv_obj_set_pos(right_eye_, right_heart_x, heart_y);
    
    // Set loving mode active
    loving_mode_active_ = true;
    
    // 左眼爱心放大缩小动画
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye_);
    lv_anim_set_values(&left_eye_anim_, 64, 128);  // 从100%放大到200%
    lv_anim_set_time(&left_eye_anim_, 500);         // 放大时间
    lv_anim_set_playback_time(&left_eye_anim_, 500);// 缩小时间
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&left_eye_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_linear);
    lv_anim_start(&left_eye_anim_);

    // 右眼爱心放大缩小动画
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_eye_);
    lv_anim_set_values(&right_eye_anim_, 64, 128);  // 从100%放大到200%
    lv_anim_set_time(&right_eye_anim_, 500);
    lv_anim_set_playback_time(&right_eye_anim_, 500);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&right_eye_anim_, (lv_anim_exec_xcb_t)lv_img_set_zoom);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_linear);
    lv_anim_start(&right_eye_anim_);
    
    // Create recovery timer for 4 seconds
    if (!loving_recovery_timer_) {
        const esp_timer_create_args_t loving_recovery_timer_args = {
            .callback = [](void* arg) {
                XunguanDisplay* self = static_cast<XunguanDisplay*>(arg);
                if (!self) {
                    return;
                }
                
                // Clear loving mode flag
                ESP_LOGI(TAG, "loving_recovery");
                self->loving_mode_active_ = false;
                
                // Stop current animation and start idle animation
                self->StopCurrentAnimation();
                const auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                if (state == kDeviceStateSleeping || state == kDeviceStateIdle) {
                    self->StartSleepingAnimation();
                } else {
                    self->StartIdleAnimation();
                }
            },
            .arg = this,
            .name = "loving_recovery"
        };
        
        esp_err_t ret = esp_timer_create(&loving_recovery_timer_args, &loving_recovery_timer_);
        if (ret != ESP_OK) {
            loving_mode_active_ = false;
            ESP_LOGE(TAG, "Failed to create loving recovery timer: %s", esp_err_to_name(ret));
            return;
        }
    }
    
    // Start 4-second timer
    esp_err_t ret = esp_timer_start_once(loving_recovery_timer_, 4000000);  // 4 seconds in microseconds
    if (ret != ESP_OK) {
        loving_mode_active_ = false;
        ESP_LOGE(TAG, "Failed to start loving recovery timer: %s", esp_err_to_name(ret));
        return;
    }
}

void XunguanDisplay::StartThinkingAnimation() {
    

    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }

    // 清理现有UI
    ClearUIElements();
    // 创建基础动画
    StartIdleAnimation();
    DisplayLockGuard lock(this);
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;

    // 创建小手图片（hand_img）
    lv_obj_t* hand = lv_img_create(screen);
    if (!hand) {
        ESP_LOGE(TAG, "Failed to create hand image!");
        return;
    }
    lv_img_set_src(hand, &hand_img);
    lv_obj_set_style_img_recolor(hand, lv_color_hex(EYE_COLOR), 0);  // Use EYE_COLOR
    lv_obj_set_style_img_recolor_opa(hand, LV_OPA_COVER, 0);
    lv_img_set_zoom(hand, 128);  // Scale down to 50%

    // 居中放在眼睛下方
    int hand_width = hand_img.header.w;
    int hand_height = hand_img.header.h;
    int hand_x = (screen_width - hand_width * 0.4) / 2 - 20;
    int hand_y = screen_height / 2 + 25;  // 向下移动15像素
    lv_obj_set_pos(hand, hand_x, hand_y);
    // 可选：设置无边框、无阴影
    lv_obj_set_style_border_width(hand, 0, 0);
    lv_obj_set_style_shadow_width(hand, 0, 0);
    lv_obj_set_style_outline_width(hand, 0, 0);
    
    // 创建轻微上下晃动的动画
    lv_anim_t hand_anim;
    lv_anim_init(&hand_anim);
    lv_anim_set_var(&hand_anim, hand);
    lv_anim_set_values(&hand_anim, hand_y, hand_y + 6);  // 上下6像素的晃动
    lv_anim_set_time(&hand_anim, 1000);  // 1秒一个周期
    lv_anim_set_exec_cb(&hand_anim, [](void* var, int32_t value) {
        lv_obj_t* obj = static_cast<lv_obj_t*>(var);
        lv_obj_set_y(obj, value);
    });
    lv_anim_set_path_cb(&hand_anim, lv_anim_path_ease_in_out);  // 使用缓动效果
    lv_anim_set_repeat_count(&hand_anim, LV_ANIM_REPEAT_INFINITE);  // 无限循环
    lv_anim_set_playback_time(&hand_anim, 1000);  // 回放时间1秒
    lv_anim_set_playback_delay(&hand_anim, 0);  // 无延迟
    lv_anim_start(&hand_anim);

}

void XunguanDisplay::StartShockedAnimation() {
    
    auto screen = lv_screen_active();
    if (!screen) {
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    StartIdleAnimation();
    DisplayLockGuard lock(this);

    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    // Create shocked mouth (circular) below the eyes
    lv_obj_t* shocked_mouth = lv_obj_create(screen);
    if (!shocked_mouth) {
        return;
    }
    
    // Calculate mouth dimensions and position
    int mouth_size = screen_width / 8;  // 1/8 of screen width for mouth
    int mouth_x = (screen_width / 2) - (mouth_size / 2);
    int mouth_y = 180;  // 20 pixels below eyes
    
    // Set mouth properties - perfect circle
    lv_obj_set_size(shocked_mouth, mouth_size, mouth_size);
    lv_obj_set_pos(shocked_mouth, mouth_x, mouth_y);
    lv_obj_set_style_radius(shocked_mouth, mouth_size / 2, 0);  // Perfect circle
    lv_obj_set_style_bg_color(shocked_mouth, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(shocked_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shocked_mouth, 0, 0);  // No border
    lv_obj_set_style_border_side(shocked_mouth, LV_BORDER_SIDE_NONE, 0);  // No border side
    lv_obj_set_style_pad_all(shocked_mouth, 0, 0);  // No padding
    lv_obj_set_style_shadow_width(shocked_mouth, 0, 0);  // No shadow
    lv_obj_set_style_outline_width(shocked_mouth, 0, 0);  // No outline
    
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
}

void XunguanDisplay::StartSleepingAnimation() {
    
    auto screen = lv_screen_active();
    if (!screen) {
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Calculate screen dimensions
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    // Create three "z" labels with absolute positioning for 240x240 circular screen
    // Positioned in the upper area to avoid the center where eyes would be
    // 
    // For a 240x240 circular screen:
    // - Center is at (120, 120)
    // - Upper area is roughly y < 100
    // - Left area is roughly x < 120  
    // - Right area is roughly x > 120
    // 
    // Positioning strategy:
    // - zzz1: Upper left (60, 50) - avoids center, visible in upper left
    // - zzz2: Upper center (113, 30) - centered horizontally, high up
    // - zzz3: Upper right (160, 30) - higher position, forms diagonal line with zzz1
    // First "z" - upper left area (60, 50)
    zzz1_ = lv_label_create(screen);
    if (!zzz1_) {
        return;
    }
    lv_label_set_text(zzz1_, "z");
    lv_obj_set_style_text_color(zzz1_, lv_color_hex(EYE_COLOR), 0);  // Pink color
    lv_obj_set_style_text_font(zzz1_, &lv_font_montserrat_22, 0);  // Use Montserrat font
    lv_obj_set_style_text_letter_space(zzz1_, 2, 0);  // Increase letter spacing
    lv_obj_set_pos(zzz1_, 60, 50);  // Upper left area for circular screen
    
    
    // Second "z" - upper center area (113, 30)
    zzz2_ = lv_label_create(screen);
    if (!zzz2_) {
        return;
    }
    lv_label_set_text(zzz2_, "z");
    lv_obj_set_style_text_color(zzz2_, lv_color_hex(EYE_COLOR), 0);  // Pink color
    lv_obj_set_style_text_font(zzz2_, &lv_font_montserrat_22, 0);  // Use Montserrat font
    lv_obj_set_style_text_letter_space(zzz2_, 2, 0);  // Increase letter spacing
    lv_obj_set_pos(zzz2_, screen_width / 2 - 7, 30);  // Upper center for circular screen
    
    
    // Third "z" - upper right area (160, 30) - higher position for diagonal line
    zzz3_ = lv_label_create(screen);
    if (!zzz3_) {
        ESP_LOGE(TAG, "Failed to create zzz3 label!");
        return;
    }
    lv_label_set_text(zzz3_, "z");
    lv_obj_set_style_text_color(zzz3_, lv_color_hex(EYE_COLOR), 0);  // Pink color
    lv_obj_set_style_text_font(zzz3_, &lv_font_montserrat_22, 0);  // Use Montserrat font
    lv_obj_set_style_text_letter_space(zzz3_, 2, 0);  // Increase letter spacing
    lv_obj_set_pos(zzz3_, screen_width - 80, 30);  // Higher position for diagonal line
    
    
    // Create two eyes below the zzz labels with squinting effect
    // Eye dimensions - X axis longer than Y axis for squinting effect
    int eye_width = screen_width / 3.5;   // 1/4 of screen width (reduced from 1/3)
    int eye_height = eye_width / 4;     // Y axis 1/4 of X axis (reduced from 1/3)
    
    // Calculate positions for centered eyes below zzz labels
    int eye_spacing = screen_width / 2.5;  // 1/2 of screen width between eyes (increased from 1/3)
    int left_eye_x = (screen_width / 2) - (eye_spacing / 2) - (eye_width / 2);
    int right_eye_x = (screen_width / 2) + (eye_spacing / 2) - (eye_width / 2);
    int eye_y = 110;  // Position below zzz labels
    
    
    // Create left eye
    left_eye_ = lv_obj_create(screen);
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left eye!");
        return;
    }
    
    // Set left eye properties for squinting effect
    lv_obj_set_size(left_eye_, eye_width, eye_height);
    lv_obj_set_pos(left_eye_, left_eye_x, eye_y);
    lv_obj_set_style_radius(left_eye_, eye_height / 2, 0);  // Rounded corners based on height
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);  // No border
    lv_obj_set_style_border_side(left_eye_, LV_BORDER_SIDE_NONE, 0);  // No border side
    lv_obj_set_style_pad_all(left_eye_, 0, 0);  // No padding
    lv_obj_set_style_shadow_width(left_eye_, 0, 0);  // No shadow
    lv_obj_set_style_outline_width(left_eye_, 0, 0);  // No outline
    
    
    // Create right eye
    right_eye_ = lv_obj_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right eye!");
        return;
    }
    
    // Set right eye properties for squinting effect
    lv_obj_set_size(right_eye_, eye_width, eye_height);
    lv_obj_set_pos(right_eye_, right_eye_x, eye_y);
    lv_obj_set_style_radius(right_eye_, eye_height / 2, 0);  // Rounded corners based on height
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);  // No border
    lv_obj_set_style_border_side(right_eye_, LV_BORDER_SIDE_NONE, 0);  // No border side
    lv_obj_set_style_pad_all(right_eye_, 0, 0);  // No padding
    lv_obj_set_style_shadow_width(right_eye_, 0, 0);  // No shadow
    lv_obj_set_style_outline_width(right_eye_, 0, 0);  // No outline
    
    
    // Start animations for the "z" labels
    StartSimpleColorAnimation(zzz1_);      // First "z" starts immediately
    StartSimpleColorAnimation(zzz2_);    // Second "z" starts after 500ms
    StartSimpleColorAnimation(zzz3_);   // Third "z" starts after 1000ms
    
    
    // StartSimpleColorAnimation(left_eye_);
    // StartSimpleColorAnimation(right_eye_);
    // Force refresh the screen
    lv_obj_invalidate(screen);
}

void XunguanDisplay::StartSillyAnimation() {
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Calculate eye dimensions and positions based on screen size
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    // Eye dimensions for silly animation
    int eye_width = screen_width / 6;   // 1/6 of screen width
    int eye_height = eye_width * 2;     // Y axis twice as long as X axis
    
    // Calculate positions for centered eyes
    int eye_spacing = screen_width / 3;  // 1/3 of screen width between eyes
    int left_eye_x = (screen_width / 2) - (eye_spacing / 2) - (eye_width / 2);
    int right_eye_x = (screen_width / 2) + (eye_spacing / 2) - (eye_width / 2);
    int eye_y = (screen_height / 2) - (eye_height / 2);
    
    
    // Create left eye
    left_eye_ = lv_obj_create(screen);
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left eye!");
        return;
    }
    
    // Set left eye properties
    lv_obj_set_size(left_eye_, eye_width, eye_height);
    lv_obj_set_pos(left_eye_, left_eye_x, eye_y);
    lv_obj_set_style_radius(left_eye_, eye_width / 2, 0);  // Rounded corners
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);  // No border
    
    
    // Create right eye
    right_eye_ = lv_obj_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right eye!");
        return;
    }
    
    // Set right eye properties
    lv_obj_set_size(right_eye_, eye_width, eye_height);
    lv_obj_set_pos(right_eye_, right_eye_x, eye_y);
    lv_obj_set_style_radius(right_eye_, eye_width / 2, 0);  // Rounded corners
    lv_obj_set_style_bg_color(right_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);  // No border
    
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    
    // Start silly height animation
    StartSillyEyeHeightAnimation(left_eye_, right_eye_);
}

void XunguanDisplay::StartVertigoAnimation() {
    
    // Set vertigo mode active
    vertigo_mode_active_ = true;
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Hide the original eyes
    if (left_eye_) {
        lv_obj_add_flag(left_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_eye_) {
        lv_obj_add_flag(right_eye_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Calculate screen dimensions
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    
    // Create left spiral image
    left_eye_ = lv_img_create(screen);
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left spiral image!");
        return;
    }
    
    // Set left spiral image properties
    lv_img_set_src(left_eye_, &spiral_img_64);
    lv_obj_set_style_img_recolor(left_eye_, lv_color_hex(EYE_COLOR), 0);  // Use EYE_COLOR
    lv_obj_set_style_img_recolor_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_align(left_eye_, LV_ALIGN_LEFT_MID, 0, 0);  // Left side, middle
    lv_img_set_zoom(left_eye_, 128);  // Scale down to 50%
    
    
    // Create right spiral image
    right_eye_ = lv_img_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right spiral image!");
        return;
    }
    
    // Set right spiral image properties
    lv_img_set_src(right_eye_, &spiral_img_64);
    lv_obj_set_style_img_recolor(right_eye_, lv_color_hex(EYE_COLOR), 0);  // Use EYE_COLOR
    lv_obj_set_style_img_recolor_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_align(right_eye_, LV_ALIGN_RIGHT_MID, 0, 0);  // Right side, middle
    lv_img_set_zoom(right_eye_, 128);  // Scale down to 50%
    
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    
    // Start rotation animation
    StartVertigoRotationAnimation(left_eye_, right_eye_);
}

void XunguanDisplay::ClearUIElements() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    if (!screen) {
        return;
    }
    
    
    // Stop all animations first
    if (left_eye_) {
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_del(left_eye_, simple_color_anim_cb);
        lv_anim_del(left_eye_, heart_zoom_anim_cb);
        lv_anim_del(left_eye_, blink_anim_cb);
        lv_anim_del(left_eye_, thinking_float_anim_cb);
        lv_anim_del(left_eye_, eye_scaling_anim_cb);
        
        // 清理眨眼动画的用户数据
        lv_anim_t* anim = lv_anim_get(left_eye_, (lv_anim_exec_xcb_t)blink_anim_cb);
        if (anim) {
            BlinkUserData* user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
            if (user_data) {
                delete user_data;
            }
        }
        
        // 清理思考浮动动画的用户数据
        anim = lv_anim_get(left_eye_, (lv_anim_exec_xcb_t)thinking_float_anim_cb);
        if (anim) {
            BlinkUserData* user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
            if (user_data) {
                delete user_data;
            }
        }
        
        // 清理眼睛缩放动画的用户数据
        anim = lv_anim_get(left_eye_, (lv_anim_exec_xcb_t)eye_scaling_anim_cb);
        if (anim) {
            BlinkUserData* user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
            if (user_data) {
                delete user_data;
            }
        }
    }
    if (right_eye_) {
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_del(right_eye_, simple_color_anim_cb);
        lv_anim_del(right_eye_, heart_zoom_anim_cb);
        lv_anim_del(right_eye_, blink_anim_cb);
        lv_anim_del(right_eye_, thinking_float_anim_cb);
        lv_anim_del(right_eye_, eye_scaling_anim_cb);
    }
    if (mouth_) {
        lv_anim_del(mouth_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    }
    
    // Clear existing objects
    if (left_eye_) {
        lv_obj_del(left_eye_);
        left_eye_ = nullptr;
    }
    if (right_eye_) {
        lv_obj_del(right_eye_);
        right_eye_ = nullptr;
    }
    if (container_) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
    if (mouth_) {
        lv_obj_del(mouth_);
        mouth_ = nullptr;
    }
    
    // Clear sleep UI elements
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
    
    // Clear all other UI elements (including mouth images)
    lv_obj_t* current_screen = lv_screen_active();
    if (current_screen) {
        uint32_t i;
        for (i = 0; i < lv_obj_get_child_cnt(current_screen); i++) {
            lv_obj_t* child = lv_obj_get_child(current_screen, i);
            // Only delete if it's not left_eye_ or right_eye_ (they're handled separately)
            if (child != left_eye_ && child != right_eye_) {
                lv_obj_del(child);
            }
        }
    }
    
    // Force a refresh after clearing
    lv_obj_invalidate(screen);
    
}

void XunguanDisplay::StartEyeScalingAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye, int original_height) {
    
    if (!left_eye || !right_eye) {
        ESP_LOGE(TAG, "Invalid eye objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(left_eye, simple_color_anim_cb);
    lv_anim_del(right_eye, simple_color_anim_cb);
    lv_anim_del(left_eye, eye_scaling_anim_cb);
    lv_anim_del(right_eye, eye_scaling_anim_cb);
    
    // Animation parameters
    int min_height = original_height * 8 / 10;  // Shrink to 8/10 (slight)
    int anim_duration = 700;  // 1 second per cycle
    
    // 使用单个动画和回调函数同时处理两个眼睛
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye);  // 使用左眼作为主对象
    lv_anim_set_values(&left_eye_anim_, original_height, min_height);
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, eye_scaling_anim_cb);  // 使用自定义回调
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    
    // 将右眼对象和原始高度存储到动画的用户数据中
    BlinkUserData* user_data = new BlinkUserData;
    user_data->right_eye = right_eye;
    user_data->original_left_y = lv_obj_get_y(left_eye);
    user_data->original_right_y = lv_obj_get_y(right_eye);
    lv_anim_set_user_data(&left_eye_anim_, user_data);
    
    lv_anim_start(&left_eye_anim_);
    
}

// Custom callback removed - using LVGL built-in lv_obj_set_height function

void XunguanDisplay::StartSimpleColorAnimation(lv_obj_t* obj) {
    
    if (!obj) {
        ESP_LOGE(TAG, "Invalid object!");
        return;
    }
    
    // Stop any existing animations on this object
    lv_anim_del(obj, simple_color_anim_cb);
    lv_anim_del(obj, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(obj, (lv_anim_exec_xcb_t)lv_obj_set_width);
    
    // Animation parameters
    int anim_duration = 2000;  // 2 seconds per cycle
    
    // Start animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, obj);
    lv_anim_set_values(&left_eye_anim_, 100, 255);  // Text opacity animation
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, simple_color_anim_cb);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_start(&left_eye_anim_);
    
    
}

void XunguanDisplay::simple_color_anim_cb(void* var, int32_t v) {
    lv_obj_t* obj = (lv_obj_t*)var;
    if (!obj) {
        return;
    }
    
    // Change text opacity instead of background opacity
    lv_obj_set_style_text_opa(obj, v, 0);
}

void XunguanDisplay::eye_scaling_anim_cb(void* var, int32_t v) {
    lv_obj_t* left_eye = (lv_obj_t*)var;
    
    // 获取用户数据
    lv_anim_t* anim = lv_anim_get(left_eye, (lv_anim_exec_xcb_t)eye_scaling_anim_cb);
    BlinkUserData* user_data = nullptr;
    
    if (anim) {
        user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
    }
    
    if (!left_eye || !user_data || !user_data->right_eye) {
        return;
    }
    
    lv_obj_t* right_eye = user_data->right_eye;
    
    // 同时设置两个眼睛的高度
    lv_obj_set_height(left_eye, v);
    lv_obj_set_height(right_eye, v);
} 

void XunguanDisplay::StartHeartScalingAnimation(lv_obj_t* left_heart, lv_obj_t* right_heart, int original_width, int original_height) {
    
    
    if (!left_heart || !right_heart) {
        ESP_LOGE(TAG, "Invalid heart objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_heart, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_del(left_heart, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_heart, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_del(right_heart, (lv_anim_exec_xcb_t)lv_obj_set_height);
    
    // Animation parameters
    int anim_duration = 1500;  // 1.5 seconds per cycle
    
    
    
    // Start left heart zoom animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_heart);
    lv_anim_set_values(&left_eye_anim_, 179/2, 179 / 3);  // 256 = 100%, 179 = 70% (fixed scaling values)
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, heart_zoom_anim_cb);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_set_user_data(&left_eye_anim_, right_heart);  // Store right heart reference
    lv_anim_start(&left_eye_anim_);
    
    
    
    // Start right heart zoom animation (with delay for alternating effect)
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_heart);
    lv_anim_set_values(&right_eye_anim_, 179 / 2, 179 / 3);  // 256 = 100%, 179 = 70% (fixed scaling values)
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&right_eye_anim_, heart_zoom_anim_cb);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim_, anim_duration);
    lv_anim_set_delay(&right_eye_anim_, anim_duration / 2);  // Half cycle delay
    lv_anim_set_user_data(&right_eye_anim_, left_heart);  // Store left heart reference
    lv_anim_start(&right_eye_anim_);
    
    
    
} 

void XunguanDisplay::heart_zoom_anim_cb(void* var, int32_t v) {
    lv_obj_t* heart = (lv_obj_t*)var;
    if (!heart) {
        ESP_LOGE(TAG, "Heart zoom animation callback: invalid heart object");
        return;
    }
    
    // Set zoom transform (v is in 256ths, so 256 = 100%)
    lv_obj_set_style_transform_zoom(heart, v, 0);
    
    // Get the other heart from user data
    lv_anim_t* anim = lv_anim_get(heart, (lv_anim_exec_xcb_t)heart_zoom_anim_cb);
    lv_obj_t* other_heart = nullptr;
    if (anim) {
        other_heart = (lv_obj_t*)lv_anim_get_user_data(anim);
    }
    
    if (!other_heart) {
        return;
    }
    
    // Calculate positions based on current scale
    float current_scale = v / 256.0f;  // Convert to 0-1 scale
    int screen_width = DISPLAY_WIDTH;
    int heart_width = 160;  // Original heart width
    int scaled_heart_width = (int)(heart_width * current_scale);
    int heart_spacing = 20;
    
    int total_hearts_width = scaled_heart_width * 2;
    int total_width = total_hearts_width + heart_spacing;
    int start_x = (screen_width - total_width) / 2;
    
    // Ensure hearts don't go outside screen boundaries
    if (start_x < 0) start_x = 0;
    if (start_x + total_width > screen_width) start_x = screen_width - total_width;
    
    // Update positions for both hearts
    int left_heart_x = start_x;
    int right_heart_x = start_x + scaled_heart_width + heart_spacing;
    
    // Get the target Y position from user data or use current Y
    int target_y = lv_obj_get_y(heart);
    
    // Determine which heart is which by comparing with the animation variable
    // The heart parameter is the one being animated, so we can use it directly
    if (heart == anim->var) {
        // This is the left heart
        lv_obj_set_pos(heart, left_heart_x, target_y);
        lv_obj_set_pos(other_heart, right_heart_x, target_y);
    } else {
        // This is the right heart
        lv_obj_set_pos(heart, right_heart_x, target_y);
        lv_obj_set_pos(other_heart, left_heart_x, target_y);
    }
    
    ESP_LOGD(TAG, "Heart zoom: scale=%.2f, scaled_width=%d, left_x=%d, right_x=%d", 
             current_scale, scaled_heart_width, left_heart_x, right_heart_x);
}

void XunguanDisplay::QueueAnimation(AnimationType type) {
    
    
    // If same animation is already pending, ignore
    if (pending_animation_.is_pending && pending_animation_.type == type) {
        
        return;
    }
    
    // Set pending animation
    pending_animation_.type = type;
    pending_animation_.is_pending = true;
    
    
    
    // Process the queue immediately
    ProcessAnimationQueue();
}

void XunguanDisplay::ProcessAnimationQueue() {
    if (!pending_animation_.is_pending) {
        return;
    }
    
    
    
    // Stop current animation first
    StopCurrentAnimation();
    
    // Start new animation
    StartAnimation(pending_animation_.type);
    
    // Clear pending flag
    pending_animation_.is_pending = false;
    
    
}

void XunguanDisplay::StopCurrentAnimation() {
    
    
    // Stop all animations first
    if (left_eye_) {
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_del(left_eye_, simple_color_anim_cb);
        lv_anim_del(left_eye_, heart_zoom_anim_cb);
        lv_anim_del(left_eye_, blink_anim_cb);
        lv_anim_del(left_eye_, thinking_float_anim_cb);
        lv_anim_del(left_eye_, eye_scaling_anim_cb);
    }
    if (right_eye_) {
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_del(right_eye_, simple_color_anim_cb);
        lv_anim_del(right_eye_, heart_zoom_anim_cb);
        lv_anim_del(right_eye_, blink_anim_cb);
        lv_anim_del(right_eye_, thinking_float_anim_cb);
        lv_anim_del(right_eye_, eye_scaling_anim_cb);
    }
    if (mouth_) {
        lv_anim_del(mouth_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    }
    
    // Clear UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);

}

void XunguanDisplay::StartAnimation(AnimationType type) {
    
    
    switch (type) {
        case AnimationType::IDLE:
            StartIdleAnimation();
            break;
        case AnimationType::HAPPY:
            StartHappyAnimation();
            break;
        case AnimationType::SAD:
            StartSadAnimation();
            break;
        case AnimationType::LOVING:
            StartLovingAnimation();
            break;
        case AnimationType::THINKING:
            StartThinkingAnimation();
            break;
        case AnimationType::SHOCKED:
            StartShockedAnimation();
            break;
        case AnimationType::SLEEPING:
            StartSleepingAnimation();
            break;
        case AnimationType::SILLY:
            StartSillyAnimation();
            break;
        case AnimationType::VERTIGO:
            StartVertigoAnimation();
            break;
        default:
            ESP_LOGW(TAG, "Unknown animation type: %d", static_cast<int>(type));
            break;
    }
} 

// Frame rate control implementation
bool XunguanDisplay::SetFrameRateMode(FrameRateMode mode) {
    
    
    uint32_t min_ms, max_ms, tick_period_us;
    
    switch (mode) {
        case FrameRateMode::POWER_SAVE:
            min_ms = 8;
            max_ms = 20;
            tick_period_us = 2700;  // 8ms tick
            
            break;
            
        case FrameRateMode::NORMAL:
            min_ms = 5;
            max_ms = 15;
            tick_period_us = 2000;  // 2ms tick
            
            break;
            
        case FrameRateMode::SMOOTH:
            min_ms = 2;
            max_ms = 8;
            tick_period_us = 1000;  // 1ms tick
            
            break;
            
        case FrameRateMode::CUSTOM:
            ESP_LOGW(TAG, "Custom mode requires SetCustomFrameRate() call");
            return false;
            
        default:
            ESP_LOGE(TAG, "Unknown frame rate mode");
            return false;
    }
    
    return UpdateFrameRateSettings(mode, min_ms, max_ms, tick_period_us);
}

bool XunguanDisplay::SetCustomFrameRate(uint32_t min_ms, uint32_t max_ms) {
    
    
    // Validate parameters
    if (min_ms < 1 || max_ms < min_ms || max_ms > 100) {
        ESP_LOGE(TAG, "Invalid custom frame rate parameters");
        return false;
    }
    
    // Calculate tick period based on min delay (tick should be <= min delay)
    uint32_t tick_period_us = MIN(min_ms * 1000, 4000);  // Max 2ms tick
    
    return UpdateFrameRateSettings(FrameRateMode::CUSTOM, min_ms, max_ms, tick_period_us);
}

bool XunguanDisplay::UpdateFrameRateSettings(FrameRateMode mode, uint32_t min_ms, uint32_t max_ms, uint32_t tick_period_us) {
    
    // Update internal variables
    current_frame_rate_mode_ = mode;
    current_min_delay_ms_ = min_ms;
    current_max_delay_ms_ = max_ms;
    current_tick_period_us_ = tick_period_us;
    
    // Apply new settings if display is initialized
    if (initialized_) {
        ApplyFrameRateSettings();
    }
    
    
    return true;
}

void XunguanDisplay::ApplyFrameRateSettings() {
    
    
    if (!lvgl_tick_timer_) {
        ESP_LOGW(TAG, "LVGL tick timer not initialized");
        return;
    }
    
    // Stop current timer
    esp_timer_stop(lvgl_tick_timer_);
    
    // Restart with new period
    esp_err_t ret = esp_timer_start_periodic(lvgl_tick_timer_, current_tick_period_us_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart LVGL tick timer: %s", esp_err_to_name(ret));
        return;
    }
} 

void XunguanDisplay::StartSillyEyeHeightAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye) {
    
    
    if (!left_eye || !right_eye) {
        ESP_LOGE(TAG, "Invalid eye objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    
    // Ensure eyes are visible
    lv_obj_clear_flag(left_eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye, LV_OBJ_FLAG_HIDDEN);
    
    // Animation parameters
    int anim_duration = 1000;  // 1 second per cycle
    int min_height = 40;        // Minimum height
    int max_height = 80;        // Maximum height
    
    
    
    // Left eye height animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye);
    lv_anim_set_values(&left_eye_anim_, min_height, max_height);
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_delay(&left_eye_anim_, 0);
    lv_anim_set_exec_cb(&left_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_set_playback_delay(&left_eye_anim_, 0);
    lv_anim_start(&left_eye_anim_);
    
    
    
    // Right eye height animation
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_eye);
    lv_anim_set_values(&right_eye_anim_, min_height, max_height);
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_delay(&right_eye_anim_, 0);
    lv_anim_set_exec_cb(&right_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim_, anim_duration);
    lv_anim_set_playback_delay(&right_eye_anim_, 0);
    lv_anim_start(&right_eye_anim_);
    
    
    
} 

void XunguanDisplay::StartVertigoRotationAnimation(lv_obj_t* left_spiral, lv_obj_t* right_spiral) {
    
    
    if (!left_spiral || !right_spiral) {
        ESP_LOGE(TAG, "Invalid spiral objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_spiral, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_del(right_spiral, (lv_anim_exec_xcb_t)lv_img_set_angle);
    
    // Animation parameters
    int anim_duration = 1500;  // 1.5 seconds per cycle
    int rotation_angle = 3600;  // 10 full rotations (360 * 10)
    
    
    
    // Left spiral rotation animation (clockwise)
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_spiral);
    lv_anim_set_values(&left_eye_anim_, 0, -rotation_angle);  // Clockwise rotation
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_delay(&left_eye_anim_, 0);
    lv_anim_set_exec_cb(&left_eye_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&left_eye_anim_);
    
    
    
    // Right spiral rotation animation (counter-clockwise)
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_spiral);
    lv_anim_set_values(&right_eye_anim_, 0, rotation_angle);  // Counter-clockwise rotation
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_delay(&right_eye_anim_, 0);
    lv_anim_set_exec_cb(&right_eye_anim_, (lv_anim_exec_xcb_t)lv_img_set_angle);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_linear);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&right_eye_anim_);
    
    
    
    
    // Create recovery timer for 4 seconds
    if (!vertigo_recovery_timer_) {
        const esp_timer_create_args_t vertigo_timer_args = {
            .callback = [](void* arg) {
                XunguanDisplay* self = static_cast<XunguanDisplay*>(arg);
                if (!self) {
                    ESP_LOGE(TAG, "Vertigo recovery timer callback: invalid self pointer");
                    return;
                }
                // Clear vertigo mode flag
                ESP_LOGI(TAG, "vertigo_recovery");
                self->vertigo_mode_active_ = false;
                // Stop current animation and start idle animation
                self->StopCurrentAnimation();
                const auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                if (state == kDeviceStateSleeping || state == kDeviceStateIdle) {
                    self->StartSleepingAnimation();
                } else {
                    self->StartIdleAnimation();
                }
            },
            .arg = this,
            .name = "vertigo_recovery"
        };
        
        esp_err_t ret = esp_timer_create(&vertigo_timer_args, &vertigo_recovery_timer_);
        if (ret != ESP_OK) {
            vertigo_mode_active_ = false;
            ESP_LOGE(TAG, "Failed to create vertigo recovery timer: %s", esp_err_to_name(ret));
            return;
        }
    }
    
    // Start 4-second timer
    esp_err_t ret = esp_timer_start_once(vertigo_recovery_timer_, 4000000);  // 4 seconds in microseconds
    if (ret != ESP_OK) {
        vertigo_mode_active_ = false;
        ESP_LOGE(TAG, "Failed to start vertigo recovery timer: %s", esp_err_to_name(ret));
        return;
    }
    
    
} 

void XunguanDisplay::StartHappyBlinkingAnimation(lv_obj_t* left_circle, lv_obj_t* right_circle, int original_size) {
    
    
    if (!left_circle || !right_circle) {
        ESP_LOGE(TAG, "Invalid circle objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_circle, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(left_circle, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_del(left_circle, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_del(left_circle, blink_anim_cb);
    lv_anim_del(right_circle, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_circle, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_del(right_circle, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_del(right_circle, blink_anim_cb);
    
    // Ensure circles are visible
    lv_obj_clear_flag(left_circle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_circle, LV_OBJ_FLAG_HIDDEN);
    
    // Animation parameters - 有间隔的眨眼效果
    int blink_duration = 200;  // 眨眼动作时长 0.2秒
    int pause_duration = 2000;  // 停顿时长 2秒
    int min_height = original_size / 4;  // 压缩到25%高度
    int max_height = original_size;  // 全高度
    
    // 计算Y坐标调整（保持眼睛居中）
    int original_y = lv_obj_get_y(left_circle);
    int min_y = original_y + (max_height - min_height) / 2;  // 压缩时向上调整Y坐标
    
    
    // 使用单个动画和回调函数同时处理两个眼睛
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_circle);  // 使用左眼作为主对象
    lv_anim_set_values(&left_eye_anim_, max_height, min_height);
    lv_anim_set_time(&left_eye_anim_, blink_duration);
    lv_anim_set_delay(&left_eye_anim_, 0);
    lv_anim_set_exec_cb(&left_eye_anim_, blink_anim_cb);  // 使用自定义回调
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, blink_duration);
    lv_anim_set_playback_delay(&left_eye_anim_, pause_duration);
    
    // 将右眼对象和原始Y坐标存储到动画的用户数据中
    BlinkUserData* user_data = new BlinkUserData;
    user_data->right_eye = right_circle;
    user_data->original_left_y = lv_obj_get_y(left_circle);
    user_data->original_right_y = lv_obj_get_y(right_circle);
    lv_anim_set_user_data(&left_eye_anim_, user_data);
    
    lv_anim_start(&left_eye_anim_);
    
    
}

void XunguanDisplay::StartThinkingFloatAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye, int original_y) {
    
    
    if (!left_eye || !right_eye) {
        ESP_LOGE(TAG, "Invalid eye objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_eye, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_del(right_eye, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_del(left_eye, thinking_float_anim_cb);
    lv_anim_del(right_eye, thinking_float_anim_cb);
    
    // Ensure eyes are visible
    lv_obj_clear_flag(left_eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_eye, LV_OBJ_FLAG_HIDDEN);
    
    // Animation parameters - 轻微的上下浮动
    int anim_duration = 2000;  // 2秒一个周期
    int float_distance = 8;     // 8像素的浮动距离
    int start_pos = 0;
    int end_pos = float_distance;
    
    
    
    // 使用单个动画和回调函数同时处理两个眼睛
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye);  // 使用左眼作为主对象
    lv_anim_set_values(&left_eye_anim_, start_pos, end_pos);
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_delay(&left_eye_anim_, 0);
    lv_anim_set_exec_cb(&left_eye_anim_, thinking_float_anim_cb);  // 使用自定义回调
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_set_playback_delay(&left_eye_anim_, 0);
    
    // 将右眼对象和原始Y坐标存储到动画的用户数据中
    BlinkUserData* user_data = new BlinkUserData;
    user_data->right_eye = right_eye;
    user_data->original_left_y = original_y;
    user_data->original_right_y = original_y;
    lv_anim_set_user_data(&left_eye_anim_, user_data);
    
    lv_anim_start(&left_eye_anim_);
    
    
}

void XunguanDisplay::StartTearFallingAnimation(lv_obj_t* left_tear, lv_obj_t* right_tear, int start_y) {
    
    
    if (!left_tear || !right_tear) {
        ESP_LOGE(TAG, "Invalid tear objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_tear, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_del(right_tear, (lv_anim_exec_xcb_t)lv_obj_set_y);
    
    // Ensure tears are visible
    lv_obj_clear_flag(left_tear, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right_tear, LV_OBJ_FLAG_HIDDEN);
    
    // Animation parameters
    int anim_duration = 1000;  // 1 second per cycle
    int fall_distance = 20;     // 20 pixels fall distance
    int start_pos = start_y;
    int end_pos = start_y + fall_distance;
    
    
    
    // Left tear falling animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_tear);
    lv_anim_set_values(&left_eye_anim_, start_pos, end_pos);
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_delay(&left_eye_anim_, 0);
    lv_anim_set_exec_cb(&left_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_set_playback_delay(&left_eye_anim_, 0);
    lv_anim_start(&left_eye_anim_);
    
    
    
    // Right tear falling animation (synchronized)
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_tear);
    lv_anim_set_values(&right_eye_anim_, start_pos, end_pos);
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_delay(&right_eye_anim_, 0);
    lv_anim_set_exec_cb(&right_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim_, anim_duration);
    lv_anim_set_playback_delay(&right_eye_anim_, 0);
    lv_anim_start(&right_eye_anim_);
    
    
    
}

void XunguanDisplay::EnterOTAMode() {
    
    
    DisplayLockGuard lock(this);
    
    // 清空屏幕
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    lv_obj_clean(screen);
    
    // 设置黑色背景
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // 创建圆环进度条
    ota_progress_bar_ = lv_arc_create(screen);
    if (!ota_progress_bar_) {
        ESP_LOGE(TAG, "Failed to create OTA progress bar!");
        return;
    }
    
    // 设置圆环大小和位置
    int screen_height = DISPLAY_HEIGHT;
    lv_obj_set_size(ota_progress_bar_, screen_height - 4, screen_height - 4);
    lv_obj_align(ota_progress_bar_, LV_ALIGN_CENTER, 0, 0);
    
    // 设置圆环属性
    lv_arc_set_value(ota_progress_bar_, 0);
    lv_arc_set_bg_angles(ota_progress_bar_, 0, 360);
    lv_arc_set_rotation(ota_progress_bar_, 270);  // 从顶部开始
    lv_obj_remove_style(ota_progress_bar_, NULL, LV_PART_KNOB);  // 去除旋钮
    lv_obj_clear_flag(ota_progress_bar_, LV_OBJ_FLAG_CLICKABLE);  // 去除可点击属性
    
    // 设置背景弧宽度和颜色
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_black(), LV_PART_MAIN);
    
    // 设置前景弧宽度和颜色（Tiffany Blue）
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_hex(EYE_COLOR), LV_PART_INDICATOR);
    
    // 创建百分比标签
    ota_number_label_ = lv_label_create(screen);
    if (!ota_number_label_) {
        ESP_LOGE(TAG, "Failed to create OTA number label!");
        return;
    }
    
    lv_obj_align(ota_number_label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ota_number_label_, "0%");
    lv_obj_set_style_text_font(ota_number_label_, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(ota_number_label_, lv_color_hex(EYE_COLOR), 0);
    
    // 重置进度
    ota_progress_ = 0;
    
    // 强制刷新屏幕
    lv_obj_invalidate(screen);
    
    
}

void XunguanDisplay::SetOTAProgress(int progress) {
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
    
    // 强制刷新屏幕
    lv_obj_invalidate(lv_screen_active());
    
    
}

void XunguanDisplay::EnterWifiConfig() {
    

    
    ClearUIElements();
    
    DisplayLockGuard lock(this);
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // 设置背景为白色
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // 删除所有子对象（清空屏幕）
    lv_obj_clean(screen);
    
    // 显示二维码图片
    lv_obj_t* img = lv_img_create(screen);
    if (!img) {
        ESP_LOGE(TAG, "Failed to create QR code image!");
        return;
    }
    
    lv_img_set_src(img, &qrcode_img);
    lv_obj_center(img);  // 居中显示
    
    // 强制刷新屏幕
    lv_obj_invalidate(screen);
    
    
}

void XunguanDisplay::blink_anim_cb(void* var, int32_t v) {
    lv_obj_t* left_eye = (lv_obj_t*)var;
    
    // 获取用户数据
    lv_anim_t* anim = lv_anim_get(left_eye, (lv_anim_exec_xcb_t)blink_anim_cb);
    BlinkUserData* user_data = nullptr;
    
    if (anim) {
        user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
    }
    
    if (!left_eye || !user_data || !user_data->right_eye) {
        ESP_LOGE(TAG, "Blink animation callback: invalid objects");
        return;
    }
    
    lv_obj_t* right_eye = user_data->right_eye;
    
    // 同时设置两个眼睛的高度
    lv_obj_set_height(left_eye, v);
    lv_obj_set_height(right_eye, v);
    
    // 计算并调整Y坐标以保持居中
    // 使用存储的原始Y坐标，避免累积误差
    int original_height = 60;
    int current_height = v;
    
    // 计算Y偏移量：当高度从60压缩到v时，眼睛中心向上移动的距离
    int y_offset = (original_height - current_height) / 2;
    
    // 基于原始Y坐标计算调整后的位置
    int adjusted_left_y = user_data->original_left_y + y_offset + 30;
    int adjusted_right_y = user_data->original_right_y + y_offset + 30;
    
    // 同时调整两个眼睛的Y坐标
    lv_obj_set_y(left_eye, adjusted_left_y);
    lv_obj_set_y(right_eye, adjusted_right_y);
}

void XunguanDisplay::thinking_float_anim_cb(void* var, int32_t v) {
    lv_obj_t* left_eye = (lv_obj_t*)var;
    
    // 获取用户数据
    lv_anim_t* anim = lv_anim_get(left_eye, (lv_anim_exec_xcb_t)thinking_float_anim_cb);
    BlinkUserData* user_data = nullptr;
    
    if (anim) {
        user_data = (BlinkUserData*)lv_anim_get_user_data(anim);
    }
    
    if (!left_eye || !user_data || !user_data->right_eye) {
        ESP_LOGE(TAG, "Thinking float animation callback: invalid objects");
        return;
    }
    
    lv_obj_t* right_eye = user_data->right_eye;
    
    // 同时设置两个眼睛的Y坐标，实现轻微的上下浮动
    lv_obj_set_y(left_eye, user_data->original_left_y + v);
    lv_obj_set_y(right_eye, user_data->original_right_y + v);
}