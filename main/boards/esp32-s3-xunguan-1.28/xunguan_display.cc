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

#define TAG "XunguanDisplay"
#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

// Static member initialization
_lock_t XunguanDisplay::lvgl_api_lock = {0};

XunguanDisplay::XunguanDisplay() 
    : Display(), pending_animation_(), animation_queue_enabled_(true),
      panel_io_(nullptr), panel_(nullptr), lvgl_display_(nullptr), initialized_(false),
      left_eye_(nullptr), container_(nullptr), current_state_(EyeState::IDLE),
      left_eye_anim_(), right_eye_anim_(), right_eye_(nullptr),
      lvgl_tick_timer_(nullptr), lvgl_task_handle_(nullptr) {
    
    // Initialize static lock
    _lock_init(&lvgl_api_lock);
}

XunguanDisplay::~XunguanDisplay() {
    if (lvgl_tick_timer_) {
        esp_timer_stop(lvgl_tick_timer_);
        esp_timer_delete(lvgl_tick_timer_);
    }
    
    if (lvgl_task_handle_) {
        vTaskDelete(lvgl_task_handle_);
    }
    
    if (lvgl_display_) {
        lv_display_delete(lvgl_display_);
    }
}

bool XunguanDisplay::Initialize() {
    ESP_LOGI(TAG, "Initializing XunguanDisplay");
    
    ESP_LOGI(TAG, "Step 1: Initialize SPI");
    if (!InitializeSpi()) {
        ESP_LOGE(TAG, "Failed to initialize SPI");
        return false;
    }
    ESP_LOGI(TAG, "SPI initialized successfully");
    
    ESP_LOGI(TAG, "Step 2: Initialize LCD panel");
    if (!InitializeLcdPanel()) {
        ESP_LOGE(TAG, "Failed to initialize LCD panel");
        return false;
    }
    ESP_LOGI(TAG, "LCD panel initialized successfully");
    
    ESP_LOGI(TAG, "Step 3: Initialize LVGL");
    if (!InitializeLvgl()) {
        ESP_LOGE(TAG, "Failed to initialize LVGL");
        return false;
    }
    ESP_LOGI(TAG, "LVGL initialized successfully");
    
    ESP_LOGI(TAG, "Step 4: Initialize LVGL timer");
    if (!InitializeLvglTimer()) {
        ESP_LOGE(TAG, "Failed to initialize LVGL timer");
        return false;
    }
    ESP_LOGI(TAG, "LVGL timer initialized successfully");
    
    ESP_LOGI(TAG, "Step 5: Create LVGL task");
    if (!CreateLvglTask()) {
        ESP_LOGE(TAG, "Failed to create LVGL task");
        return false;
    }
    ESP_LOGI(TAG, "LVGL task created successfully");
    
    ESP_LOGI(TAG, "Step 6: Setup UI");
    // Setup UI after everything is initialized
    SetupUI();
    ESP_LOGI(TAG, "UI setup completed");
    
    // Force a manual refresh immediately
    ESP_LOGI(TAG, "Forcing manual refresh");
    lv_refr_now(lvgl_display_);
    
    // Start idle animation after initialization
    ESP_LOGI(TAG, "Starting idle animation");
    StartIdleAnimation();
    
    // Add a small delay to ensure everything is ready
    vTaskDelay(pdMS_TO_TICKS(100));
    
    initialized_ = true;
    ESP_LOGI(TAG, "XunguanDisplay initialization completed");
    return true;
}

bool XunguanDisplay::InitializeSpi() {
    ESP_LOGI(TAG, "Initialize SPI bus");
    
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
    ESP_LOGI(TAG, "Install panel IO");
    
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
    ESP_LOGI(TAG, "Initialize LVGL library");
    
    lv_init();
    
    // Create LVGL display
    lvgl_display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!lvgl_display_) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return false;
    }
    
    ESP_LOGI(TAG, "LVGL display created successfully: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    
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
    ESP_LOGI(TAG, "Install LVGL tick timer");
    
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
    
    ret = esp_timer_start_periodic(lvgl_tick_timer_, 2000);  // 2ms period
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL tick timer: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "LVGL tick timer started successfully");
    return true;
}

bool XunguanDisplay::CreateLvglTask() {
    ESP_LOGI(TAG, "Create LVGL task");
    
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
    
    ESP_LOGI(TAG, "LVGL task created successfully");
    return true;
}

void XunguanDisplay::lvgl_task(void* arg) {
    ESP_LOGI(TAG, "Starting LVGL task");
    
    uint32_t time_till_next_ms = 0;
    uint32_t loop_count = 0;
    
    // Log immediately to confirm task started
    ESP_LOGI(TAG, "LVGL task entered main loop");
    
    while (1) {
        
        // Minimize lock holding time
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        
        // Ensure minimum delay to prevent excessive CPU usage
        time_till_next_ms = MAX(time_till_next_ms, 5);  // At least 5ms
        time_till_next_ms = MIN(time_till_next_ms, 15); // Cap at 15ms
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
    
    ESP_LOGI(TAG, "Setting up basic UI");
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    ESP_LOGI(TAG, "Screen found, setting background to black");
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    // Force a manual refresh
    lv_refr_now(lvgl_display_);
    
    ESP_LOGI(TAG, "Basic UI setup completed - background set to black");
}

void XunguanDisplay::SetEmotion(const char* emotion) {
    ESP_LOGI(TAG, "SetEmotion called: %s", emotion);
    
    if (!animation_queue_enabled_) {
        ESP_LOGW(TAG, "Animation queue is disabled");
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
    
    ESP_LOGI(TAG, "StartIdleAnimation called - creating two eyes");
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "ClearUIElements called");
    
    // Set background to black
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    ESP_LOGI(TAG, "Screen background set to black");
    // Create left eye
    left_eye_ = lv_obj_create(screen);
    ESP_LOGI(TAG, "Left eye created: %p", left_eye_);
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
    
    ESP_LOGI(TAG, "Screen: %dx%d, Eye: %dx%d, Positions: L(%d,%d) R(%d,%d)", 
             screen_width, screen_height, eye_width, eye_height, left_eye_x, eye_y, right_eye_x, eye_y);
    
    int y_offset = 2;
    // Set left eye properties
    lv_obj_set_size(left_eye_, eye_width, eye_height);
    lv_obj_set_pos(left_eye_, left_eye_x, eye_y - y_offset);
    lv_obj_set_style_radius(left_eye_, eye_width / 2, 0);  // Rounded corners
    lv_obj_set_style_bg_color(left_eye_, lv_color_hex(EYE_COLOR), 0);
    lv_obj_set_style_bg_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);  // No border
    
    ESP_LOGI(TAG, "Left eye created: size=%dx%d, pos=(%d,%d)", eye_width, eye_height, left_eye_x, eye_y);
    
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
    
    ESP_LOGI(TAG, "Right eye created: size=%dx%d, pos=(%d,%d)", eye_width, eye_height, right_eye_x, eye_y);
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    ESP_LOGI(TAG, "Two eyes created successfully - starting Y-axis animation");
    
    // Start Y-axis scaling animation for both eyes
    ESP_LOGI(TAG, "Eyes created, left_eye_: %p, right_eye_: %p, height: %d", left_eye_, right_eye_, eye_height);
    StartEyeScalingAnimation(left_eye_, right_eye_, eye_height);
}

void XunguanDisplay::StartHappyAnimation() {
    ESP_LOGI(TAG, "StartHappyAnimation called");
}

void XunguanDisplay::StartSadAnimation() {
    ESP_LOGI(TAG, "StartSadAnimation called");
}

void XunguanDisplay::StartLovingAnimation() {
    ESP_LOGI(TAG, "StartLovingAnimation called - creating two hearts");
    
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
    
    // Calculate heart dimensions and positions based on screen size
    int screen_width = DISPLAY_WIDTH;
    int screen_height = DISPLAY_HEIGHT;
    
    // Heart dimensions - read from hart_img
    int heart_width = hart_img.header.w;   // Read width from image
    int heart_height = hart_img.header.h;  // Read height from image
    
    // Calculate positions for centered hearts
    int heart_spacing = screen_width / 3;  // 1/3 of screen width between hearts
    int left_heart_x = (screen_width / 2) - (heart_spacing / 2) - (heart_width / 2);
    int right_heart_x = (screen_width / 2) + (heart_spacing / 2) - (heart_width / 2);
    int heart_y = (screen_height / 2) - (heart_height / 2);
    
    ESP_LOGI(TAG, "Screen: %dx%d, Heart: %dx%d, Positions: L(%d,%d) R(%d,%d)", 
             screen_width, screen_height, heart_width, heart_height, left_heart_x, heart_y, right_heart_x, heart_y);
    
    // Create left heart image
    left_eye_ = lv_img_create(screen);
    if (!left_eye_) {
        ESP_LOGE(TAG, "Failed to create left heart image!");
        return;
    }
    
    // Set heart image properties
    lv_img_set_src(left_eye_, &hart_img);
    lv_obj_set_style_img_recolor(left_eye_, lv_color_hex(EYE_COLOR), 0); 
    lv_obj_set_style_img_recolor_opa(left_eye_, LV_OPA_COVER, 0);
    lv_obj_set_size(left_eye_, heart_width, heart_height);
    lv_obj_set_pos(left_eye_, left_heart_x, heart_y);
    
    ESP_LOGI(TAG, "Left heart created: size=%dx%d, pos=(%d,%d)", heart_width, heart_height, left_heart_x, heart_y);
    
    // Create right heart image
    right_eye_ = lv_img_create(screen);
    if (!right_eye_) {
        ESP_LOGE(TAG, "Failed to create right heart image!");
        return;
    }
    
    // Set heart image properties
    lv_img_set_src(right_eye_, &hart_img);
    lv_obj_set_style_img_recolor(right_eye_, lv_color_hex(EYE_COLOR), 0); 
    lv_obj_set_style_img_recolor_opa(right_eye_, LV_OPA_COVER, 0);
    lv_obj_set_size(right_eye_, heart_width, heart_height);
    lv_obj_set_pos(right_eye_, right_heart_x, heart_y);
    
    ESP_LOGI(TAG, "Right heart created: size=%dx%d, pos=(%d,%d)", heart_width, heart_height, right_heart_x, heart_y);
    
    // Force refresh the screen
    lv_obj_invalidate(screen);
    
    ESP_LOGI(TAG, "Two hearts created successfully");
    
    // Start heart scaling animation
    StartHeartScalingAnimation(left_eye_, right_eye_, heart_width * 0.5, heart_height * 0.5);
}

void XunguanDisplay::StartThinkingAnimation() {
    ESP_LOGI(TAG, "StartThinkingAnimation called");
}

void XunguanDisplay::StartShockedAnimation() {
    ESP_LOGI(TAG, "StartShockedAnimation called");
}

void XunguanDisplay::StartSleepingAnimation() {
    ESP_LOGI(TAG, "StartSleepingAnimation called");
    
    auto screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "No active screen found!");
        return;
    }
    
    // Clear existing UI elements
    ClearUIElements();
    
    
}

void XunguanDisplay::StartSillyAnimation() {
    ESP_LOGI(TAG, "StartSillyAnimation called");
}

void XunguanDisplay::StartVertigoAnimation() {
    ESP_LOGI(TAG, "StartVertigoAnimation called");
}

void XunguanDisplay::ClearUIElements() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    if (!screen) {
        return;
    }
    
    ESP_LOGI(TAG, "Clearing UI elements");
    
    // Stop all animations first
    if (left_eye_) {
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(left_eye_, simple_color_anim_cb);
        lv_anim_del(left_eye_, heart_zoom_anim_cb);
    }
    if (right_eye_) {
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(right_eye_, simple_color_anim_cb);
        lv_anim_del(right_eye_, heart_zoom_anim_cb);
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
    
    // Force a refresh after clearing
    lv_obj_invalidate(screen);
    
    ESP_LOGI(TAG, "UI elements cleared");
}

void XunguanDisplay::StartEyeScalingAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye, int original_height) {
    ESP_LOGI(TAG, "Starting eye scaling animation - left: %p, right: %p, height: %d", left_eye, right_eye, original_height);
    
    if (!left_eye || !right_eye) {
        ESP_LOGE(TAG, "Invalid eye objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(left_eye, simple_color_anim_cb);
    lv_anim_del(right_eye, simple_color_anim_cb);
    
    // Animation parameters - very conservative for testing
    int min_height = original_height * 8 / 10;  // Shrink to 8/10 (slight)
    int anim_duration = 1000;  // 4 seconds per cycle (slow)
    
    ESP_LOGI(TAG, "Animation params - min_height: %d, duration: %d", min_height, anim_duration);
    
    // Start left eye animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye);
    lv_anim_set_values(&left_eye_anim_, original_height, min_height);
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_start(&left_eye_anim_);
    
    ESP_LOGI(TAG, "Left eye animation started");
    
    // Start right eye animation (no delay)
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_eye);
    lv_anim_set_values(&right_eye_anim_, original_height, min_height);
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&right_eye_anim_, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim_, anim_duration);
    lv_anim_start(&right_eye_anim_);
    
    ESP_LOGI(TAG, "Right eye animation started");
    ESP_LOGI(TAG, "Eye scaling animation started - height range: %d to %d", min_height, original_height);
}

// Custom callback removed - using LVGL built-in lv_obj_set_height function

void XunguanDisplay::StartSimpleColorAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye) {
    ESP_LOGI(TAG, "Starting simple color animation");
    
    if (!left_eye || !right_eye) {
        ESP_LOGE(TAG, "Invalid eye objects!");
        return;
    }
    
    // Stop any existing animations on these objects
    lv_anim_del(left_eye, simple_color_anim_cb);
    lv_anim_del(right_eye, simple_color_anim_cb);
    lv_anim_del(left_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_del(right_eye, (lv_anim_exec_xcb_t)lv_obj_set_height);
    
    // Animation parameters
    int anim_duration = 2000;  // 2 seconds per cycle
    
    // Start left eye animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_eye);
    lv_anim_set_values(&left_eye_anim_, 0, 255);  // Opacity animation
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, simple_color_anim_cb);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_start(&left_eye_anim_);
    
    ESP_LOGI(TAG, "Simple color animation started");
}

void XunguanDisplay::simple_color_anim_cb(void* var, int32_t v) {
    lv_obj_t* eye = (lv_obj_t*)var;
    if (!eye) {
        ESP_LOGE(TAG, "Color animation callback: invalid eye object");
        return;
    }
    
    // Simple opacity animation
    lv_obj_set_style_bg_opa(eye, v, 0);
    
    // Debug log every 20th call to avoid spam
    static int call_count = 0;
    call_count++;
    if (call_count % 20 == 0) {
        ESP_LOGI(TAG, "Color animation callback: eye %p, opacity: %ld", eye, (long)v);
    }
} 

void XunguanDisplay::StartHeartScalingAnimation(lv_obj_t* left_heart, lv_obj_t* right_heart, int original_width, int original_height) {
    ESP_LOGI(TAG, "Starting heart scaling animation - left: %p, right: %p, size: %dx%d", 
             left_heart, right_heart, original_width, original_height);
    
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
    int anim_duration = 800;  // 1.5 seconds per cycle
    
    ESP_LOGI(TAG, "Animation params - duration: %d", anim_duration);
    
    // Start left heart zoom animation
    lv_anim_init(&left_eye_anim_);
    lv_anim_set_var(&left_eye_anim_, left_heart);
    lv_anim_set_values(&left_eye_anim_, 256 / 2, 179 / 2);  // 256 = 100%, 179 = 70%
    lv_anim_set_time(&left_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&left_eye_anim_, heart_zoom_anim_cb);
    lv_anim_set_path_cb(&left_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&left_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&left_eye_anim_, anim_duration);
    lv_anim_start(&left_eye_anim_);
    
    ESP_LOGI(TAG, "Left heart animation started");
    
    // Start right heart zoom animation (with delay for alternating effect)
    lv_anim_init(&right_eye_anim_);
    lv_anim_set_var(&right_eye_anim_, right_heart);
    lv_anim_set_values(&right_eye_anim_, 256 / 2, 179 / 2);  // 256 = 100%, 179 = 70%
    lv_anim_set_time(&right_eye_anim_, anim_duration);
    lv_anim_set_exec_cb(&right_eye_anim_, heart_zoom_anim_cb);
    lv_anim_set_path_cb(&right_eye_anim_, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&right_eye_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_time(&right_eye_anim_, anim_duration);
    lv_anim_set_delay(&right_eye_anim_, anim_duration / 2);  // Half cycle delay
    lv_anim_start(&right_eye_anim_);
    
    ESP_LOGI(TAG, "Right heart animation started");
    ESP_LOGI(TAG, "Heart zoom animation started - zoom range: 70%% to 100%%");
} 

void XunguanDisplay::heart_zoom_anim_cb(void* var, int32_t v) {
    lv_obj_t* heart = (lv_obj_t*)var;
    if (!heart) {
        ESP_LOGE(TAG, "Heart zoom animation callback: invalid heart object");
        return;
    }
    
    // Set zoom transform (v is in 256ths, so 256 = 100%)
    lv_obj_set_style_transform_zoom(heart, v, 0);
} 

void XunguanDisplay::QueueAnimation(AnimationType type) {
    ESP_LOGI(TAG, "Queueing animation: %d", static_cast<int>(type));
    
    // If same animation is already pending, ignore
    if (pending_animation_.is_pending && pending_animation_.type == type) {
        ESP_LOGI(TAG, "Same animation already pending, ignoring");
        return;
    }
    
    // Set pending animation
    pending_animation_.type = type;
    pending_animation_.is_pending = true;
    
    ESP_LOGI(TAG, "Animation queued: %d", static_cast<int>(type));
    
    // Process the queue immediately
    ProcessAnimationQueue();
}

void XunguanDisplay::ProcessAnimationQueue() {
    if (!pending_animation_.is_pending) {
        return;
    }
    
    ESP_LOGI(TAG, "Processing animation queue: %d", static_cast<int>(pending_animation_.type));
    
    // Stop current animation first
    StopCurrentAnimation();
    
    // Start new animation
    StartAnimation(pending_animation_.type);
    
    // Clear pending flag
    pending_animation_.is_pending = false;
    
    ESP_LOGI(TAG, "Animation queue processed");
}

void XunguanDisplay::StopCurrentAnimation() {
    ESP_LOGI(TAG, "Stopping current animation");
    
    // Stop all animations first
    if (left_eye_) {
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(left_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(left_eye_, simple_color_anim_cb);
        lv_anim_del(left_eye_, heart_zoom_anim_cb);
    }
    if (right_eye_) {
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_del(right_eye_, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_del(right_eye_, simple_color_anim_cb);
        lv_anim_del(right_eye_, heart_zoom_anim_cb);
    }
    
    // Clear UI elements
    ClearUIElements();
    DisplayLockGuard lock(this);

}

void XunguanDisplay::StartAnimation(AnimationType type) {
    ESP_LOGI(TAG, "Starting animation: %d", static_cast<int>(type));
    
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