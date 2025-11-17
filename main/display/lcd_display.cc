#include "lcd_display.h"

#include <vector>
#include <algorithm>
#include <string>
#include <font_awesome_symbols.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include "assets/lang_config.h"
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include "settings.h"
#include "ntp.h"
#include "device_state_event.h"
#include <esp_spiffs.h>
#include <esp_vfs.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <lvgl.h>

#include "board.h"

#define TAG "LcdDisplay"

// Background SPIFFS partition configuration
#define BACKGROUND_PARTITION_LABEL "background"
#define BACKGROUND_MOUNT_POINT "/background"
#define BACKGROUND_DRIVE_LETTER 'B'

// External background images
extern const lv_image_dsc_t bg_1_img;
extern const lv_image_dsc_t bg_2_img;

// Color definitions for dark theme
#define DARK_BACKGROUND_COLOR       lv_color_hex(0x121212)     // Dark background
#define DARK_TEXT_COLOR             lv_color_white()           // White text
#define DARK_CHAT_BACKGROUND_COLOR  lv_color_hex(0x1E1E1E)     // Slightly lighter than background
#define DARK_USER_BUBBLE_COLOR      lv_color_hex(0x1A6C37)     // Dark green
#define DARK_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x333333)     // Dark gray
#define DARK_SYSTEM_BUBBLE_COLOR    lv_color_hex(0x2A2A2A)     // Medium gray
#define DARK_SYSTEM_TEXT_COLOR      lv_color_hex(0xAAAAAA)     // Light gray text
#define DARK_BORDER_COLOR           lv_color_hex(0x333333)     // Dark gray border
#define DARK_LOW_BATTERY_COLOR      lv_color_hex(0xFF0000)     // Red for dark mode

// Color definitions for light theme
#define LIGHT_BACKGROUND_COLOR       lv_color_white()           // White background
#define LIGHT_TEXT_COLOR             lv_color_black()           // Black text
#define LIGHT_CHAT_BACKGROUND_COLOR  lv_color_hex(0xE0E0E0)     // Light gray background
#define LIGHT_USER_BUBBLE_COLOR      lv_color_hex(0x95EC69)     // WeChat green
#define LIGHT_ASSISTANT_BUBBLE_COLOR lv_color_white()           // White
#define LIGHT_SYSTEM_BUBBLE_COLOR    lv_color_hex(0xE0E0E0)     // Light gray
#define LIGHT_SYSTEM_TEXT_COLOR      lv_color_hex(0x666666)     // Dark gray text
#define LIGHT_BORDER_COLOR           lv_color_hex(0xE0E0E0)     // Light gray border
#define LIGHT_LOW_BATTERY_COLOR      lv_color_black()           // Black for light mode


// Define dark theme colors
const ThemeColors DARK_THEME = {
    .background = DARK_BACKGROUND_COLOR,
    .text = DARK_TEXT_COLOR,
    .chat_background = DARK_CHAT_BACKGROUND_COLOR,
    .user_bubble = DARK_USER_BUBBLE_COLOR,
    .assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = DARK_SYSTEM_BUBBLE_COLOR,
    .system_text = DARK_SYSTEM_TEXT_COLOR,
    .border = DARK_BORDER_COLOR,
    .low_battery = DARK_LOW_BATTERY_COLOR
};

// Define light theme colors
const ThemeColors LIGHT_THEME = {
    .background = LIGHT_BACKGROUND_COLOR,
    .text = LIGHT_TEXT_COLOR,
    .chat_background = LIGHT_CHAT_BACKGROUND_COLOR,
    .user_bubble = LIGHT_USER_BUBBLE_COLOR,
    .assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR,
    .system_text = LIGHT_SYSTEM_TEXT_COLOR,
    .border = LIGHT_BORDER_COLOR,
    .low_battery = LIGHT_LOW_BATTERY_COLOR
};


LV_FONT_DECLARE(font_awesome_30_4);

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, DisplayFonts fonts, int width, int height)
    : panel_io_(panel_io), panel_(panel), fonts_(fonts) {
    width_ = width;
    height_ = height;

    // Load theme from settings
    Settings settings("display", false);
    current_theme_name_ = settings.GetString("theme", "light");

    // Update the theme
    if (current_theme_name_ == "dark") {
        current_theme_ = DARK_THEME;
    } else if (current_theme_name_ == "light") {
        current_theme_ = LIGHT_THEME;
    }
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 40),
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
    RegisterDeviceStateCallback();
    
    // Mount background SPIFFS partition and register LVGL file system driver
    LoadBackgroundFromSPIFFS();
}

// RGB LCD实现
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
    RegisterDeviceStateCallback();
    
    // Mount background SPIFFS partition and register LVGL file system driver
    LoadBackgroundFromSPIFFS();
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy,
                            DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = panel_io,
            .panel_handle = panel,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(width_ * 50),
            .double_buffer = false,
            .hres = static_cast<uint32_t>(width_),
            .vres = static_cast<uint32_t>(height_),
            .monochrome = false,
            /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
            .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
    RegisterDeviceStateCallback();
    
    // Mount background SPIFFS partition and register LVGL file system driver
    LoadBackgroundFromSPIFFS();
}

// Add background_image_ and chat_container_ as member variables (temporary storage)
static lv_obj_t* background_image_ = nullptr;
static lv_obj_t* chat_container_ = nullptr;

LcdDisplay::~LcdDisplay() {
    // Clean up countdown timer first
    StopCountdown();
    
    // Clean up subtitle scroll timer
    StopSubtitleScroll();
    
    // Clean up video playback task
    StopVideoPlayback();
    
    // 然后再清理 LVGL 对象
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
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

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
    lv_obj_set_style_border_color(container_, current_theme_.border, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme_.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
    lv_obj_set_style_pad_top(status_bar_, 2, 0);  // Add top padding to avoid being cut off
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 10, 0);
    lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0); // Background for chat area
    lv_obj_set_style_border_color(content_, current_theme_.border, 0); // Border color for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, 10, 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 10, 0);
    lv_obj_set_style_pad_right(status_bar_, 10, 0);
    lv_obj_set_style_pad_top(status_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(status_bar_, 2, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    // 设置状态栏的内容垂直居中
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建emotion_label_在状态栏最左侧
    emotion_label_ = lv_label_create(status_bar_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    lv_obj_set_style_margin_right(emotion_label_, 5, 0); // 添加右边距，与后面的元素分隔
    lv_obj_set_style_translate_y(emotion_label_, 10, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_set_style_pad_left(notification_label_, 20, 0);
    lv_obj_set_style_pad_right(notification_label_, 20, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(notification_label_, 15, 0);  // 与 status_label_ 保持一致

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    // 使用省略号模式，文字过长时显示省略号而不是被顶上去
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_set_style_translate_y(status_label_, 15, 0);
    
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);
    lv_obj_set_style_translate_y(mute_label_, 10, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);
    lv_obj_set_style_margin_left(network_label_, 5, 0); // 添加左边距，与前面的元素分隔
    lv_obj_set_style_translate_y(network_label_, 10, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);
    lv_obj_set_style_margin_left(battery_label_, 5, 0); // 添加左边距，与前面的元素分隔
    lv_obj_set_style_translate_y(battery_label_, 10, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    //避免出现空的消息框
    if(strlen(content) == 0) return;
    
    // 检查消息数量是否超过限制
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // 删除最早的消息（第一个子对象）
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
        }
        // Scroll to the last message immediately
        if (last_child != nullptr) {
            lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
        }
    }
    
    // 折叠系统消息（如果是系统消息，检查最后一个消息是否也是系统消息）
    if (strcmp(role, "system") == 0 && child_count > 0) {
        // 获取最后一个消息容器
        lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
        if (last_container != nullptr && lv_obj_get_child_cnt(last_container) > 0) {
            // 获取容器内的气泡
            lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
            if (last_bubble != nullptr) {
                // 检查气泡类型是否为系统消息
                void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                    // 如果最后一个消息也是系统消息，则删除它
                    lv_obj_del(last_container);
                }
            }
        }
    }
    
    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 1, 0);
    lv_obj_set_style_border_color(msg_bubble, current_theme_.border, 0);
    lv_obj_set_style_pad_all(msg_bubble, 8, 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // 计算文本实际宽度
    lv_coord_t text_width = lv_txt_get_width(content, strlen(content), fonts_.text_font, 0);

    // 计算气泡宽度
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 屏幕宽度的85%
    lv_coord_t min_width = 20;  
    lv_coord_t bubble_width;
    
    // 确保文本宽度不小于最小宽度
    if (text_width < min_width) {
        text_width = min_width;
    }

    // 如果文本宽度小于最大宽度，使用文本宽度
    if (text_width < max_width) {
        bubble_width = text_width; 
    } else {
        bubble_width = max_width;
    }
    
    // 设置消息文本的宽度
    lv_obj_set_width(msg_text, bubble_width);  // 减去padding
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg_text, fonts_.text_font, 0);

    // 设置气泡宽度
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.user_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.assistant_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.system_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.system_text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // 为系统消息创建全宽容器以确保居中对齐
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // 使容器透明且无边框
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // 将消息气泡移入此容器
        lv_obj_set_parent(msg_bubble, container);
        
        // 将气泡居中对齐在容器中
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        
        // 自动滚动底部
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}
#else
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);

    /* Content - make it full screen */
    content_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_size(content_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 5, 0);
    lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    
    /* Background image - try to load from SPIFFS first, fallback to embedded image */
    background_image_ = lv_image_create(content_);
    lv_obj_set_size(background_image_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(background_image_, -5, -5);  // Adjust position to cover padding
    lv_obj_clear_flag(background_image_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(background_image_, LV_OBJ_FLAG_FLOATING);  // Make it floating, not part of flex layout
    lv_obj_move_background(background_image_);  // Move to background layer
    
    // Try to load from SPIFFS first
    char bg_path[64];
    snprintf(bg_path, sizeof(bg_path), "%c:/bg.jpg", BACKGROUND_DRIVE_LETTER);
    // Check if file exists using SPIFFS path directly
    char spiffs_path[128];
    snprintf(spiffs_path, sizeof(spiffs_path), "%s/bg.jpg", BACKGROUND_MOUNT_POINT);
    FILE* f = fopen(spiffs_path, "r");
    if (f != nullptr) {
        fclose(f);
        // File exists, use LVGL path format
        ESP_LOGI(TAG, "Loading background image from SPIFFS: %s", bg_path);
        lv_image_set_src(background_image_, bg_path);
    } else {
        // Fallback to embedded image
        ESP_LOGI(TAG, "SPIFFS background image not found, using embedded image");
        lv_image_set_src(background_image_, &bg_1_img);
    }

    /* Status bar - floating on top */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height + 20);
    lv_obj_set_pos(status_bar_, 0, 2);  // Position at top with 2px offset to avoid being cut off
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_90, 0); 
    lv_obj_set_style_bg_color(status_bar_, lv_color_white(), 0);  // White background
    lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);

    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN); // 垂直布局（从上到下）
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY); // 子对象居中对齐，等距分布

    // 创建时间显示容器（带白色透明背景和圆角）
    lv_obj_t* time_container = lv_obj_create(content_);
    lv_obj_set_size(time_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(time_container, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(time_container, LV_OPA_90, 0);  // 白色半透明，增加不透明度
    lv_obj_set_style_radius(time_container, 15, 0);  // 圆角
    lv_obj_set_style_pad_all(time_container, 20, 0);  // 内边距
    lv_obj_set_style_border_width(time_container, 0, 0);  // 无边框
    lv_obj_add_flag(time_container, LV_OBJ_FLAG_FLOATING);  // 不受 flex 布局影响
    lv_obj_align(time_container, LV_ALIGN_BOTTOM_MID, 0, -10);  // 底部居中，距离底部10px
    lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
    lv_obj_move_foreground(time_container);  // Ensure it's on top of background
    
    emotion_label_ = lv_label_create(time_container);
    lv_obj_set_style_text_font(emotion_label_, fonts_.text_font, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
    lv_label_set_text(emotion_label_, "00:00:00");  // Initial countdown text
    lv_obj_center(emotion_label_);  // 在容器中居中

    preview_image_ = lv_image_create(content_);
    lv_obj_set_size(preview_image_, width_ * 0.5, height_ * 0.5);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // 创建字幕容器（带白色透明背景和圆角）
    chat_container_ = lv_obj_create(content_);
    lv_obj_set_size(chat_container_, LV_HOR_RES * 0.9, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(chat_container_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chat_container_, LV_OPA_90, 0);  // 白色半透明
    lv_obj_set_style_radius(chat_container_, 10, 0);  // 圆角
    lv_obj_set_style_pad_all(chat_container_, 15, 0);  // 内边距
    lv_obj_set_style_border_width(chat_container_, 0, 0);  // 无边框
    lv_obj_add_flag(chat_container_, LV_OBJ_FLAG_FLOATING);  // 不受 flex 布局影响
    lv_obj_align(chat_container_, LV_ALIGN_BOTTOM_MID, 0, -10);  // 底部居中，距离底部10px
    lv_obj_add_flag(chat_container_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏，只有有内容时才显示
    
    chat_message_label_ = lv_label_create(chat_container_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_PCT(100)); // 使用容器的全部宽度
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_CLIP); // 设置为裁剪模式，手动控制滚动
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0); // 设置文本左对齐
    lv_obj_set_style_text_color(chat_message_label_, current_theme_.text, 0);

    /* Status bar layout */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 5, 0);
    lv_obj_set_style_pad_left(status_bar_, 5, 0);  // Less padding needed with translate
    lv_obj_set_style_pad_right(status_bar_, 5, 0); // Less padding needed with translate

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);
    // Move network icon inward to avoid left rounded corner
    lv_obj_set_style_translate_x(network_label_, 25, 0);  // Move 5px to the right
    lv_obj_set_style_translate_y(network_label_, 14, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_set_style_pad_left(notification_label_, 20, 0);
    lv_obj_set_style_pad_right(notification_label_, 20, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_y(notification_label_, 15, 0);  // 与 status_label_ 保持一致

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    // 使用省略号模式，文字过长时显示省略号而不是被顶上去
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_set_style_translate_y(status_label_, 15, 0);
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);
    lv_obj_set_style_translate_y(mute_label_, 15, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);
    // Move battery icon inward to avoid right rounded corner
    lv_obj_set_style_translate_x(battery_label_, -30, 0);  // Move 5px to the left
    lv_obj_set_style_translate_y(battery_label_, 15, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    // Don't hide if countdown is active
    if (countdown_active_) {
        ESP_LOGD(TAG, "SetEmotion called but countdown is active, ignoring");
        return;
    }
    
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    
    // Hide time container (which contains emotion_label_) and preview_image_ only if countdown is not active
    lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
    if (time_container != nullptr) {
        lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetIcon(const char* icon) {
    // Don't hide if countdown is active
    if (countdown_active_) {
        ESP_LOGD(TAG, "SetIcon called but countdown is active, ignoring");
        return;
    }
    
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    
    // Hide time container (which contains emotion_label_) and preview_image_ only if countdown is not active
    lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
    if (time_container != nullptr) {
        lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetPreviewImage(const lv_img_dsc_t* img_dsc) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }
    
    if (img_dsc != nullptr) {
        // zoom factor 0.5
        lv_img_set_zoom(preview_image_, 128 * width_ / img_dsc->header.w);
        // 设置图片源并显示预览图片
        lv_img_set_src(preview_image_, img_dsc);
        lv_obj_clear_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        // 隐藏时间容器（包含 emotion_label_）
        if (emotion_label_ != nullptr) {
            lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
            if (time_container != nullptr) {
                lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        // 隐藏预览图片并显示时间容器（包含 emotion_label_）
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        if (emotion_label_ != nullptr) {
            lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
            if (time_container != nullptr) {
                lv_obj_clear_flag(time_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LcdDisplay::SetTheme(const std::string& theme_name) {
    DisplayLockGuard lock(this);
    
    if (theme_name == "dark" || theme_name == "DARK") {
        current_theme_ = DARK_THEME;
    } else if (theme_name == "light" || theme_name == "LIGHT") {
        current_theme_ = LIGHT_THEME;
    } else {
        // Invalid theme name, return false
        ESP_LOGE(TAG, "Invalid theme name: %s", theme_name.c_str());
        return;
    }
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();
    
    // Update the screen colors
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    
    // Update container colors
    if (container_ != nullptr) {
        lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
        lv_obj_set_style_border_color(container_, current_theme_.border, 0);
    }
    
    // Update status bar colors
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_color(status_bar_, current_theme_.background, 0);
        lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
        
        // Update status bar elements
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);
        }
        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
        }
    }
    
    // Update content area colors
    if (content_ != nullptr) {
        lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0);
        lv_obj_set_style_border_color(content_, current_theme_.border, 0);
        
        // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        // Iterate through all children of content (message containers or bubbles)
        uint32_t child_count = lv_obj_get_child_cnt(content_);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* obj = lv_obj_get_child(content_, i);
            if (obj == nullptr) continue;
            
            lv_obj_t* bubble = nullptr;
            
            // 检查这个对象是容器还是气泡
            // 如果是容器（用户或系统消息），则获取其子对象作为气泡
            // 如果是气泡（助手消息），则直接使用
            if (lv_obj_get_child_cnt(obj) > 0) {
                // 可能是容器，检查它是否为用户或系统消息容器
                // 用户和系统消息容器是透明的
                lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, 0);
                if (bg_opa == LV_OPA_TRANSP) {
                    // 这是用户或系统消息的容器
                    bubble = lv_obj_get_child(obj, 0);
                } else {
                    // 这可能是助手消息的气泡自身
                    bubble = obj;
                }
            } else {
                // 没有子元素，可能是其他UI元素，跳过
                continue;
            }
            
            if (bubble == nullptr) continue;
            
            // 使用保存的用户数据来识别气泡类型
            void* bubble_type_ptr = lv_obj_get_user_data(bubble);
            if (bubble_type_ptr != nullptr) {
                const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
                
                // 根据气泡类型应用正确的颜色
                if (strcmp(bubble_type, "user") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.user_bubble, 0);
                } else if (strcmp(bubble_type, "assistant") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.assistant_bubble, 0); 
                } else if (strcmp(bubble_type, "system") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.system_bubble, 0);
                }
                
                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme_.border, 0);
                
                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 根据气泡类型设置文本颜色
                        if (strcmp(bubble_type, "system") == 0) {
                            lv_obj_set_style_text_color(text, current_theme_.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme_.text, 0);
                        }
                    }
                }
            } else {
                // 如果没有标记，回退到之前的逻辑（颜色比较）
                // ...保留原有的回退逻辑...
                lv_color_t bg_color = lv_obj_get_style_bg_color(bubble, 0);
            
                // 改进bubble类型检测逻辑，不仅使用颜色比较
                bool is_user_bubble = false;
                bool is_assistant_bubble = false;
                bool is_system_bubble = false;
            
                // 检查用户bubble
                if (lv_color_eq(bg_color, DARK_USER_BUBBLE_COLOR) || 
                    lv_color_eq(bg_color, LIGHT_USER_BUBBLE_COLOR) ||
                    lv_color_eq(bg_color, current_theme_.user_bubble)) {
                    is_user_bubble = true;
                }
                // 检查系统bubble
                else if (lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) || 
                         lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR) ||
                         lv_color_eq(bg_color, current_theme_.system_bubble)) {
                    is_system_bubble = true;
                }
                // 剩余的都当作助手bubble处理
                else {
                    is_assistant_bubble = true;
                }
            
                // 根据bubble类型应用正确的颜色
                if (is_user_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.user_bubble, 0);
                } else if (is_assistant_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.assistant_bubble, 0);
                } else if (is_system_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.system_bubble, 0);
                }
                
                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme_.border, 0);
                
                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 回退到颜色检测逻辑
                        if (lv_color_eq(bg_color, current_theme_.system_bubble) ||
                            lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) || 
                            lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR)) {
                            lv_obj_set_style_text_color(text, current_theme_.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme_.text, 0);
                        }
                    }
                }
            }
        }
#else
        // Simple UI mode - just update the main chat message
        if (chat_message_label_ != nullptr) {
            lv_obj_set_style_text_color(chat_message_label_, current_theme_.text, 0);
        }
        
        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
        }
#endif
    }
    
    // Update low battery popup
    if (low_battery_popup_ != nullptr) {
        lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    }

    // No errors occurred. Save theme to settings
    Display::SetTheme(theme_name);
}

void LcdDisplay::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus called with: %s", status);
    
    // Call parent implementation
    Display::SetStatus(status);
    
    // No longer handle clock display here - now handled by SetSocketConnected
}

void LcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    
    // 调用父类方法显示通知
    Display::ShowNotification(notification, duration_ms);
    
    // 当显示通知时，隐藏时间容器
    if (emotion_label_ != nullptr && countdown_active_) {
        lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
        if (time_container != nullptr) {
            lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::UpdateStatusBar(bool update_all) {
    // 调用父类方法更新状态栏
    Display::UpdateStatusBar(update_all);
    
    // 检查低电量弹窗是否显示，如果显示则隐藏时间容器
    if (emotion_label_ != nullptr && countdown_active_) {
        if (low_battery_popup_ != nullptr && !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
            if (time_container != nullptr) {
                lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || chat_container_ == nullptr) {
        return;
    }
    
    // Show/hide container based on whether content is empty
    if (content == nullptr || strlen(content) == 0) {
        // Hide container when content is empty
        StopSubtitleScroll();
        lv_obj_add_flag(chat_container_, LV_OBJ_FLAG_HIDDEN);
        subtitle_text_.clear();
        subtitle_role_.clear();
        return;
    }
    
    // Clean up newline characters and replace with spaces
    std::string cleaned_content(content);
    std::replace(cleaned_content.begin(), cleaned_content.end(), '\n', ' ');
    std::replace(cleaned_content.begin(), cleaned_content.end(), '\r', ' ');
    
    // Remove multiple consecutive spaces
    std::string final_content;
    bool prev_space = false;
    for (char c : cleaned_content) {
        if (c == ' ') {
            if (!prev_space) {
                final_content += c;
            }
            prev_space = true;
        } else {
            final_content += c;
            prev_space = false;
        }
    }
    
    // Trim leading and trailing spaces
    if (!final_content.empty()) {
        size_t start = final_content.find_first_not_of(" \t");
        if (start != std::string::npos) {
            size_t end = final_content.find_last_not_of(" \t");
            final_content = final_content.substr(start, end - start + 1);
        } else {
            final_content.clear();
        }
    }
    
    // Store old text and role before updating (for append detection)
    std::string old_text = subtitle_text_;
    std::string old_role = subtitle_role_;
    std::string current_role = role;
    
    // Check if role has changed - if so, reset scroll state
    bool role_changed = (current_role != old_role);
    if (role_changed) {
        StopSubtitleScroll();
        subtitle_scroll_pos_ = 0;
    }
    
    // Check if this is an append operation (for user/assistant role with incremental text)
    bool is_append = false;
    if ((strcmp(role, "user") == 0 || strcmp(role, "assistant") == 0) && 
        !old_text.empty() && 
        current_role == old_role &&  // Same role
        !role_changed &&  // Role hasn't changed
        final_content.length() > old_text.length()) {
        // Check if new text starts with old text
        std::string prefix = final_content.substr(0, old_text.length());
        if (prefix == old_text) {
            // New text is longer and starts with old text - it's an append
            is_append = true;
        }
    }
    
    // Update current role
    subtitle_role_ = current_role;
    
    // Store the cleaned content for scrolling
    subtitle_text_ = final_content;
    
    // Set initial text (will be updated by scroll timer)
    if (subtitle_text_.empty()) {
        lv_obj_add_flag(chat_container_, LV_OBJ_FLAG_HIDDEN);
        subtitle_scroll_pos_ = 0;
        return;
    }
    
    // Show container
    lv_obj_clear_flag(chat_container_, LV_OBJ_FLAG_HIDDEN);
    
    // Get container width to determine if scrolling is needed (subtract padding like UpdateSubtitleDisplay)
    lv_coord_t container_width = lv_obj_get_width(chat_container_);
    if (container_width <= 0) {
        container_width = LV_HOR_RES * 0.9; // Use default width if not set
    }
    // Subtract horizontal padding (left + right) to match UpdateSubtitleDisplay logic
    lv_coord_t pad_left = lv_obj_get_style_pad_left(chat_container_, 0);
    lv_coord_t pad_right = lv_obj_get_style_pad_right(chat_container_, 0);
    container_width = container_width - pad_left - pad_right;
    
    // Calculate text width
    lv_coord_t text_width = lv_txt_get_width(subtitle_text_.c_str(), subtitle_text_.length(), fonts_.text_font, 0);
    
    // For append operations, preserve scroll state
    if (is_append) {
        // If text was already scrolling, keep scrolling
        // If text wasn't scrolling but now needs to, start scrolling
        if (!subtitle_scrolling_) {
            // Wasn't scrolling before, check if we need to start now
            if (text_width > container_width + 2) {
                // Text grew and now needs scrolling
                // For append operations, start scrolling immediately (no delay)
                // Always start from old text end for append operations, so new content appears naturally
                subtitle_scroll_pos_ = old_text.length();
                StartSubtitleScrollDelayed(); // Start immediately without delay
            }
            // If still fits, don't start scrolling
        } else {
            // Was already scrolling, continue scrolling naturally from current position
            // Don't adjust scroll_pos_ to avoid jitter - let it continue naturally
            // Continue scrolling - scroll_pos_ will increment naturally by timer until end of text
        }
    } else {
        // Not an append
        // Check if text is identical - if so, keep current scroll state
        if (final_content == old_text && !old_text.empty()) {
            // Don't reset scroll state, just update display
        } else {
            // Text changed - reset scroll position
            // Stop any existing scroll first
            StopSubtitleScroll();
            
            subtitle_scroll_pos_ = 0;
            
            // Check if scrolling is needed
            if (text_width > container_width + 2) {
                StartSubtitleScroll();
            } else {
                subtitle_scrolling_ = false;
            }
        }
    }
    
    // Update display with current text
    UpdateSubtitleDisplay();
}

void LcdDisplay::SetSocketConnected(bool connected) {
    ESP_LOGI(TAG, "SetSocketConnected: %s", connected ? "connected" : "disconnected");
    
    if (!connected) {
        // Show clock when socket is not connected
        ESP_LOGI(TAG, "Socket disconnected, showing clock");
        StartIdleCountdown();
        // ShowBackgroundImage();

    } else {
        // Hide clock when socket is connected
        ESP_LOGI(TAG, "Socket connected, hiding clock");
        StopIdleCountdown();
        // PlayVideoGroup(0);  // 播放第0组视频

    }
}

void LcdDisplay::StartIdleCountdown() {
    ESP_LOGI(TAG, "Starting clock display");
    DisplayLockGuard lock(this);
    
    if (countdown_active_) {
        StopCountdown();
    }
    
    countdown_active_ = true;
    
    // Show emotion_label_ for clock display
    if (emotion_label_ != nullptr) {
        // Make sure we have a valid font
        if (fonts_.text_font != nullptr) {
            lv_obj_set_style_text_font(emotion_label_, fonts_.text_font, 0);
        }
        
        // 只有在 NTP 同步成功时才显示时间容器（包括框框）
        lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
        if (time_container != nullptr) {
            if (NtpClient::GetInstance().IsSynced()) {
                lv_obj_clear_flag(time_container, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(time_container);  // Move to front when showing
            } else {
                // NTP 未同步，隐藏时间容器（包括框框）
                lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
        
        UpdateCountdownDisplay();  // Update immediately to show current time or placeholder
        ESP_LOGI(TAG, "Clock display initialized on emotion_label_, font: %p, NTP synced: %d", 
                 fonts_.text_font, NtpClient::GetInstance().IsSynced());
        
        // Debug: check if label is visible
        bool is_hidden = lv_obj_has_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "emotion_label_ hidden flag: %d", is_hidden);
    } else {
        ESP_LOGW(TAG, "emotion_label_ is null!");
    }
    
    // Hide preview image and chat message
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (chat_message_label_ != nullptr) {
        // 隐藏字幕容器（包含 chat_message_label_）
        lv_obj_t* chat_container = lv_obj_get_parent(chat_message_label_);
        if (chat_container != nullptr) {
            lv_obj_add_flag(chat_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    StartCountdown();  // Start timer to update clock every second
}

void LcdDisplay::StopIdleCountdown() {
    ESP_LOGI(TAG, "Stopping clock display");
    StopCountdown();
    
    // Hide the clock display and show chat message
    DisplayLockGuard lock(this);
    if (emotion_label_ != nullptr) {
        // 隐藏时间容器（包含 emotion_label_）
        lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
        if (time_container != nullptr) {
            lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (chat_message_label_ != nullptr) {
        // 显示字幕容器（包含 chat_message_label_）
        lv_obj_t* chat_container = lv_obj_get_parent(chat_message_label_);
        if (chat_container != nullptr) {
            lv_obj_clear_flag(chat_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::StartCountdown() {
    if (countdown_timer_ != nullptr) {
        esp_timer_stop(countdown_timer_);
        esp_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
    }
    
    esp_timer_create_args_t timer_args = {
        .callback = CountdownTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "countdown_timer",
        .skip_unhandled_events = true,
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &countdown_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create countdown timer: %s", esp_err_to_name(err));
        return;
    }
    
    // Start timer with 1 second period
    err = esp_timer_start_periodic(countdown_timer_, 1000000); // 1 second = 1,000,000 microseconds
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start countdown timer: %s", esp_err_to_name(err));
        esp_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
    }
}

void LcdDisplay::StopCountdown() {
    countdown_active_ = false;
    
    if (countdown_timer_ != nullptr) {
        esp_timer_stop(countdown_timer_);
        esp_timer_delete(countdown_timer_);
        countdown_timer_ = nullptr;
    }
}

void LcdDisplay::UpdateCountdownDisplay() {
    if (emotion_label_ == nullptr || !countdown_active_) {
        return;
    }
    
    // 检查是否有通知或错误显示（低电量弹窗）
    bool has_notification = false;
    if (notification_label_ != nullptr && !lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN)) {
        has_notification = true;
    }
    if (low_battery_popup_ != nullptr && !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
        has_notification = true;
    }
    
    // 如果有通知或错误，隐藏时间容器
    lv_obj_t* time_container = lv_obj_get_parent(emotion_label_);
    if (time_container != nullptr) {
        if (has_notification) {
            lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }
    
    // 检查 NTP 是否已同步，只有同步成功才显示时间
    if (!NtpClient::GetInstance().IsSynced()) {
        // NTP 未同步，隐藏时间容器（包括框框）
        if (time_container != nullptr) {
            lv_obj_add_flag(time_container, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGD(TAG, "NTP not synced, hiding time container");
        return;
    }
    
    // NTP 已同步，显示时间容器
    if (time_container != nullptr) {
        lv_obj_clear_flag(time_container, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Get current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // Format time as HH:MM:SS
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", 
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    
    lv_label_set_text(emotion_label_, time_str);
    ESP_LOGD(TAG, "Clock updated: %s", time_str);
}

void LcdDisplay::CountdownTimerCallback(void* arg) {
    LcdDisplay* display = static_cast<LcdDisplay*>(arg);
    
    if (!display->countdown_active_) {
        return;
    }
    
    // Update clock display in LVGL context
    if (display->Lock(100)) {
        display->UpdateCountdownDisplay();
        display->Unlock();
    }
}

void LcdDisplay::VideoPlayTask(void* arg) {
    auto* self = static_cast<LcdDisplay*>(arg);
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "video");
    if (!part) {
        ESP_LOGE(TAG, "video partition not found");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // Read header: 1 byte count + N*4 bytes frame counts
    uint8_t group_count = 0;
    if (esp_partition_read(part, 0, &group_count, 1) != ESP_OK || group_count == 0) {
        ESP_LOGE(TAG, "invalid video header");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    std::vector<uint32_t> frame_counts(group_count, 0);
    if (esp_partition_read(part, 1, frame_counts.data(), group_count * sizeof(uint32_t)) != ESP_OK) {
        ESP_LOGE(TAG, "read frame counts failed");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // Compute offsets
    const uint32_t frame_size = self->width_ * self->height_ * 2;
    uint32_t data_offset = 1 + group_count * sizeof(uint32_t);
    std::vector<uint32_t> group_base(group_count, 0);
    uint32_t acc_frames = 0;
    for (int i = 0; i < group_count; ++i) {
        group_base[i] = data_offset + acc_frames * frame_size;
        acc_frames += frame_counts[i];
    }
    
    int g = self->video_group_index_;
    if (g < 0 || g >= group_count) g = 0;
    uint32_t frames = frame_counts[g];
    ESP_LOGI(TAG, "Video header: groups=%u, frame_size=%u, data_offset=%u, play_group=%d, frames_in_group=%u, group_base=%u",
             (unsigned)group_count, (unsigned)frame_size, (unsigned)data_offset, g, (unsigned)frames, (unsigned)group_base[g]);
    
    if (frames == 0) {
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // Allocate frame buffer
    // Prefer DMA-capable internal memory for SPI DMA
    uint8_t* buf = nullptr;
    
    // Check if first frame buffer was provided by PlayVideoGroup (to avoid flicker)
    if (self->first_frame_buf_ != nullptr) {
        // Use the pre-loaded first frame buffer
        buf = self->first_frame_buf_;
        self->first_frame_buf_ = nullptr;  // Clear the pointer, task now owns it
        ESP_LOGI(TAG, "Using pre-loaded first frame buffer");
    } else {
        // Allocate new buffer
        buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
        if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_INTERNAL);
        if (!buf) buf = (uint8_t*)malloc(frame_size);
        if (!buf) {
            ESP_LOGE(TAG, "no memory for frame buffer");
            self->video_playing_ = false;
            vTaskDelete(nullptr);
            return;
        }
        
        // Read first frame
        uint32_t idx = 0;
        size_t off0 = group_base[g] + idx * frame_size;
        if (esp_partition_read(part, off0, buf, frame_size) != ESP_OK) {
            ESP_LOGE(TAG, "read first frame %u failed", (unsigned int)idx);
            free(buf);
            self->video_playing_ = false;
            vTaskDelete(nullptr);
            return;
        }
    }
    
    // First frame is already displayed (either pre-loaded or just read)
    // Update the image descriptor to use our buffer if needed
    uint32_t idx = 0;
    {
        if (self->Lock(50)) {
            if (self->video_img_ == nullptr) {
                // Create video image in content area (replace background_image_)
                if (self->content_ != nullptr) {
                    self->video_img_ = lv_image_create(self->content_);
                    lv_obj_set_size(self->video_img_, self->width_, self->height_);
                    lv_obj_set_pos(self->video_img_, -5, -5);  // Adjust position to cover padding
                    lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_add_flag(self->video_img_, LV_OBJ_FLAG_FLOATING);
                    lv_obj_move_background(self->video_img_);  // Move to background layer
                }
            }
            
            // Update image descriptor with current buffer (only if not already set)
            if (self->video_img_dsc_.data != buf) {
                self->video_img_dsc_.header.w = self->width_;
                self->video_img_dsc_.header.h = self->height_;
                self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
                self->video_img_dsc_.data = buf;
                self->video_img_dsc_.data_size = frame_size;
                lv_img_set_src(self->video_img_, &self->video_img_dsc_);
                lv_obj_move_background(self->video_img_);
                lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
            }
            self->Unlock();
        }
        
        vTaskDelay(pdMS_TO_TICKS(self->kVideoFrameDelayMs));
        idx = (idx + 1) % frames;
    }
    
    while (self->video_playing_) {
        size_t off = group_base[g] + idx * frame_size;
        if (esp_partition_read(part, off, buf, frame_size) != ESP_OK) {
            ESP_LOGE(TAG, "read frame %u failed", (unsigned int)idx);
            break;
        }
        
        // Short lock per frame, update LVGL image (reduce lock time to avoid blocking audio task)
        if (self->Lock(20)) {  // Reduced to 20ms for faster lock release
            self->video_img_dsc_.header.w = self->width_;
            self->video_img_dsc_.header.h = self->height_;
            self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            self->video_img_dsc_.data = buf;
            self->video_img_dsc_.data_size = frame_size;
            lv_img_set_src(self->video_img_, &self->video_img_dsc_);
            self->Unlock();
        }
        
        if ((idx % 10) == 0) {
            ESP_LOGI(TAG, "Playing group=%d idx=%u/%u off=%u", g, (unsigned)idx, (unsigned)frames, (unsigned)off);
        }
        
        vTaskDelay(pdMS_TO_TICKS(self->kVideoFrameDelayMs));
        idx = (idx + 1) % frames; // Loop play current group until key switch
    }
    
    free(buf);
    self->video_playing_ = false;
    // Clean up task handle, allow subsequent start of new playback task
    self->video_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void LcdDisplay::StartVideoPlayback() {
    // If there's already a task running, stop and wait for exit
    if (video_task_handle_ != nullptr) {
        video_playing_ = false;
        for (int i = 0; i < 50 && video_task_handle_ != nullptr; ++i) { // Wait up to 500ms
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        video_task_handle_ = nullptr;
    }
    
    ESP_LOGI(TAG, "StartVideoPlayback group=%d", video_group_index_);
    video_playing_ = true;
    // Lower priority from 5 to 1, avoid blocking audio task (audio task usually priority 3-4)
    xTaskCreate(VideoPlayTask, "video_play", 4096, this, 1, &video_task_handle_);
}

void LcdDisplay::StopVideoPlayback() {
    if (!video_playing_ && video_task_handle_ == nullptr) return;
    video_playing_ = false;
    // Wait for task to self-delete and clean up handle
    for (int i = 0; i < 50 && video_task_handle_ != nullptr; ++i) { // Wait up to 500ms
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    video_task_handle_ = nullptr;
}

void LcdDisplay::ShowBackgroundImage() {
    ESP_LOGI(TAG, "ShowBackgroundImage called");
    
    // Stop video playback if playing (outside lock to avoid deadlock)
    if (video_playing_) {
        StopVideoPlayback();
    }
    
    DisplayLockGuard lock(this);
    
    // Hide video image if exists
    if (video_img_ != nullptr) {
        lv_obj_add_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Try to load background image from SPIFFS first
    if (background_image_ != nullptr) {
        // Try to load from SPIFFS (JPG/PNG)
        char bg_path[64];
        snprintf(bg_path, sizeof(bg_path), "%c:/bg.jpg", BACKGROUND_DRIVE_LETTER);
        
        // Check if file exists using SPIFFS path directly
        char spiffs_path[128];
        snprintf(spiffs_path, sizeof(spiffs_path), "%s/bg.jpg", BACKGROUND_MOUNT_POINT);
        FILE* f = fopen(spiffs_path, "r");
        if (f != nullptr) {
            fclose(f);
            // File exists, use LVGL path format
            ESP_LOGI(TAG, "Loading background image from SPIFFS: %s", bg_path);
            lv_image_set_src(background_image_, bg_path);
            lv_obj_clear_flag(background_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(background_image_);
        } else {
            // Fallback to embedded image
            ESP_LOGI(TAG, "SPIFFS background image not found, using embedded image");
            lv_image_set_src(background_image_, &bg_1_img);
            lv_obj_clear_flag(background_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(background_image_);
        }
    }
}

void LcdDisplay::PlayVideoGroup(int index) {
    ESP_LOGI(TAG, "PlayVideoGroup called with index=%d", index);
    
    // Validate index by reading partition
    const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "video");
    if (!part) {
        ESP_LOGE(TAG, "video partition not found");
        return;
    }
    
    uint8_t group_count = 0;
    if (esp_partition_read(part, 0, &group_count, 1) != ESP_OK || group_count == 0) {
        ESP_LOGE(TAG, "invalid video header or no groups");
        return;
    }
    
    if (index < 0 || index >= group_count) {
        ESP_LOGE(TAG, "Invalid video group index %d, valid range: 0-%d", index, group_count - 1);
        return;
    }
    
    // Stop any existing playback first
    if (video_playing_) {
        StopVideoPlayback();
    }
    
    // Set group index
    video_group_index_ = index;
    
    // Read frame counts to calculate first frame offset
    std::vector<uint32_t> frame_counts(group_count, 0);
    if (esp_partition_read(part, 1, frame_counts.data(), group_count * sizeof(uint32_t)) != ESP_OK) {
        ESP_LOGE(TAG, "read frame counts failed");
        return;
    }
    
    // Compute offsets
    const uint32_t frame_size = width_ * height_ * 2;
    uint32_t data_offset = 1 + group_count * sizeof(uint32_t);
    std::vector<uint32_t> group_base(group_count, 0);
    uint32_t acc_frames = 0;
    for (int i = 0; i < group_count; ++i) {
        group_base[i] = data_offset + acc_frames * frame_size;
        acc_frames += frame_counts[i];
    }
    
    int g = index;
    if (g < 0 || g >= group_count) g = 0;
    uint32_t frames = frame_counts[g];
    
    if (frames == 0) {
        ESP_LOGE(TAG, "No frames in group %d", g);
        return;
    }
    
    // Allocate temporary buffer for first frame
    uint8_t* first_frame_buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!first_frame_buf) first_frame_buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (!first_frame_buf) first_frame_buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_INTERNAL);
    if (!first_frame_buf) first_frame_buf = (uint8_t*)malloc(frame_size);
    if (!first_frame_buf) {
        ESP_LOGE(TAG, "no memory for first frame buffer");
        return;
    }
    
    // Read first frame
    size_t first_frame_offset = group_base[g];
    if (esp_partition_read(part, first_frame_offset, first_frame_buf, frame_size) != ESP_OK) {
        ESP_LOGE(TAG, "read first frame failed");
        free(first_frame_buf);
        return;
    }
    
    // Display first frame immediately to avoid flicker
    {
        DisplayLockGuard lock(this);
        
        // Create video image if not exists
        if (video_img_ == nullptr) {
            if (content_ != nullptr) {
                video_img_ = lv_image_create(content_);
                lv_obj_set_size(video_img_, width_, height_);
                lv_obj_set_pos(video_img_, -5, -5);
                lv_obj_clear_flag(video_img_, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(video_img_, LV_OBJ_FLAG_FLOATING);
                lv_obj_move_background(video_img_);
            }
        }
        
        if (video_img_ != nullptr) {
            // Set first frame to video image
            video_img_dsc_.header.w = width_;
            video_img_dsc_.header.h = height_;
            video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            video_img_dsc_.data = first_frame_buf;
            video_img_dsc_.data_size = frame_size;
            lv_img_set_src(video_img_, &video_img_dsc_);
            lv_obj_move_background(video_img_);
            lv_obj_clear_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
            
            // Hide background image after first frame is displayed
            if (background_image_ != nullptr) {
                lv_obj_add_flag(background_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }  // Lock released here
    
    // Save first frame buffer to member variable - video task will free it after taking over
    first_frame_buf_ = first_frame_buf;
    
    // Start playback task (it will use first_frame_buf_ if available, then free it)
    StartVideoPlayback();
}

void LcdDisplay::RegisterDeviceStateCallback() {
    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState previous_state, DeviceState current_state) {
            ESP_LOGI(TAG, "Device state changed: %d -> %d", previous_state, current_state);
            
            if (current_state == kDeviceStateRealSpeaking) {
                // 说话中播放视频
                ESP_LOGI(TAG, "Speaking state detected, starting video playback");
                PlayVideoGroup(0);  // 播放第0组视频
            } else if (
                current_state == kDeviceStateListening ||
                current_state == kDeviceStateIdle ||
                current_state == kDeviceStateFatalError ||
                current_state == kDeviceStateSleeping
            ) {
                // 听话中显示桌面
                ShowBackgroundImage();
            }
        }
    );
}

void LcdDisplay::UpdateSubtitleDisplay() {
    if (chat_message_label_ == nullptr || subtitle_text_.empty()) {
        return;
    }
    
    // Get container width (subtract padding)
    lv_coord_t container_width = lv_obj_get_width(chat_container_);
    if (container_width <= 0) {
        container_width = LV_HOR_RES * 0.9;
    }
    // Subtract horizontal padding (left + right)
    lv_coord_t pad_left = lv_obj_get_style_pad_left(chat_container_, 0);
    lv_coord_t pad_right = lv_obj_get_style_pad_right(chat_container_, 0);
    container_width = container_width - pad_left - pad_right;
    
    if (subtitle_scrolling_) {
        // Scrolling mode: show substring starting from scroll position
        size_t text_len = subtitle_text_.length();
        size_t start_pos = subtitle_scroll_pos_;
        
        // If we've scrolled past the end, show the last part of the text
        if (start_pos >= text_len) {
            start_pos = text_len;
        }
        
        
        // Calculate how many characters can fit in container
        int max_chars = 0;
        lv_coord_t current_width = 0;
        for (size_t i = 0; i < subtitle_text_.length(); i++) {
            lv_coord_t char_width = lv_txt_get_width(&subtitle_text_[i], 1, fonts_.text_font, 0);
            if (current_width + char_width > container_width) {
                break;
            }
            current_width += char_width;
            max_chars++;
        }
        
        if (max_chars == 0) {
            max_chars = 1; // At least show one character
        }
        
        // Adjust start_pos if needed
        if (start_pos >= text_len) {
            start_pos = (text_len > max_chars) ? (text_len - max_chars) : 0;
        }
        
        // Show substring from start_pos
        std::string display_text = subtitle_text_.substr(start_pos);
        
        // Truncate to fit container
        current_width = 0;
        size_t display_len = 0;
        for (size_t i = 0; i < display_text.length(); i++) {
            lv_coord_t char_width = lv_txt_get_width(&display_text[i], 1, fonts_.text_font, 0);
            if (current_width + char_width > container_width) {
                break;
            }
            current_width += char_width;
            display_len++;
        }
        
        if (display_len < display_text.length()) {
            display_text = display_text.substr(0, display_len);
        }
        
        lv_label_set_text(chat_message_label_, display_text.c_str());
    } else {
        // Static mode: show full text or truncated
        lv_coord_t text_width = lv_txt_get_width(subtitle_text_.c_str(), subtitle_text_.length(), fonts_.text_font, 0);
        
        if (text_width > container_width) {
            // Truncate text to fit
            int max_chars = 0;
            lv_coord_t current_width = 0;
            for (size_t i = 0; i < subtitle_text_.length(); i++) {
                lv_coord_t char_width = lv_txt_get_width(&subtitle_text_[i], 1, fonts_.text_font, 0);
                if (current_width + char_width > container_width) {
                    break;
                }
                current_width += char_width;
                max_chars++;
            }
            if (max_chars > 0) {
                lv_label_set_text(chat_message_label_, subtitle_text_.substr(0, max_chars).c_str());
            } else {
                lv_label_set_text(chat_message_label_, subtitle_text_.substr(0, 1).c_str());
            }
        } else {
            lv_label_set_text(chat_message_label_, subtitle_text_.c_str());
        }
    }
}

void LcdDisplay::StartSubtitleScroll() {
    // Stop any existing timers
    if (subtitle_scroll_delay_timer_ != nullptr) {
        esp_timer_stop(subtitle_scroll_delay_timer_);
        esp_timer_delete(subtitle_scroll_delay_timer_);
        subtitle_scroll_delay_timer_ = nullptr;
    }
    
    if (subtitle_scroll_timer_ != nullptr) {
        esp_timer_stop(subtitle_scroll_timer_);
        esp_timer_delete(subtitle_scroll_timer_);
        subtitle_scroll_timer_ = nullptr;
    }
    
    // Create delay timer to wait before starting scroll
    esp_timer_create_args_t delay_timer_args = {
        .callback = SubtitleScrollDelayTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "subtitle_scroll_delay_timer",
        .skip_unhandled_events = true,
    };
    
    esp_err_t err = esp_timer_create(&delay_timer_args, &subtitle_scroll_delay_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create subtitle scroll delay timer: %s", esp_err_to_name(err));
        return;
    }
    
    // Start delay timer (one-shot)
    err = esp_timer_start_once(subtitle_scroll_delay_timer_, kSubtitleScrollDelayMs * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start subtitle scroll delay timer: %s", esp_err_to_name(err));
        esp_timer_delete(subtitle_scroll_delay_timer_);
        subtitle_scroll_delay_timer_ = nullptr;
    }
}

void LcdDisplay::StartSubtitleScrollDelayed() {
    // This is called after the delay period
    if (subtitle_scroll_timer_ != nullptr) {
        esp_timer_stop(subtitle_scroll_timer_);
        esp_timer_delete(subtitle_scroll_timer_);
        subtitle_scroll_timer_ = nullptr;
    }
    
    subtitle_scrolling_ = true;
    
    esp_timer_create_args_t timer_args = {
        .callback = SubtitleScrollTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "subtitle_scroll_timer",
        .skip_unhandled_events = true,
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &subtitle_scroll_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create subtitle scroll timer: %s", esp_err_to_name(err));
        subtitle_scrolling_ = false;
        return;
    }
    
    // Start timer with configurable period (default: 100ms = 10 pixels per second at 1 char per pixel)
    // kSubtitleScrollPeriodMs is defined in header, default to 100ms
    err = esp_timer_start_periodic(subtitle_scroll_timer_, kSubtitleScrollPeriodMs * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start subtitle scroll timer: %s", esp_err_to_name(err));
        esp_timer_delete(subtitle_scroll_timer_);
        subtitle_scroll_timer_ = nullptr;
        subtitle_scrolling_ = false;
    }
}

void LcdDisplay::StopSubtitleScroll() {
    subtitle_scrolling_ = false;
    
    // Stop and delete delay timer
    if (subtitle_scroll_delay_timer_ != nullptr) {
        esp_timer_stop(subtitle_scroll_delay_timer_);
        esp_timer_delete(subtitle_scroll_delay_timer_);
        subtitle_scroll_delay_timer_ = nullptr;
    }
    
    // Stop and delete scroll timer
    if (subtitle_scroll_timer_ != nullptr) {
        esp_timer_stop(subtitle_scroll_timer_);
        esp_timer_delete(subtitle_scroll_timer_);
        subtitle_scroll_timer_ = nullptr;
    }
    
    // Don't reset scroll_pos_ here - keep it so append operations can continue from the right position
    // Only reset it when starting a new non-append message
}

void LcdDisplay::SubtitleScrollTimerCallback(void* arg) {
    LcdDisplay* display = static_cast<LcdDisplay*>(arg);
    
    if (!display->subtitle_scrolling_ || display->subtitle_text_.empty()) {
        return;
    }
    
    size_t text_len = display->subtitle_text_.length();
    size_t current_pos = display->subtitle_scroll_pos_;
    
    // Check if we've reached the end of the text
    if (current_pos >= text_len) {
        // Stop scrolling when we reach the end
        display->StopSubtitleScroll();
        return;
    }
    
    // Increment scroll position
    display->subtitle_scroll_pos_++;
    
    // Update display in LVGL context
    if (display->Lock(50)) {
        display->UpdateSubtitleDisplay();
        display->Unlock();
    }
}

void LcdDisplay::SubtitleScrollDelayTimerCallback(void* arg) {
    LcdDisplay* display = static_cast<LcdDisplay*>(arg);
    
    // Clean up delay timer
    if (display->subtitle_scroll_delay_timer_ != nullptr) {
        esp_timer_delete(display->subtitle_scroll_delay_timer_);
        display->subtitle_scroll_delay_timer_ = nullptr;
    }
    
    // Start actual scrolling after delay
    display->StartSubtitleScrollDelayed();
}

// LVGL file system driver callbacks for SPIFFS
static void* fs_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    (void)drv;
    const char* flags = "r";
    if (mode == LV_FS_MODE_WR) flags = "wb";
    else if (mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) flags = "rb+";
    
    // Convert LVGL path (e.g., "B:/bg.jpg") to SPIFFS path (e.g., "/background/bg.jpg")
    char spiffs_path[128];
    if (path[0] == BACKGROUND_DRIVE_LETTER && path[1] == ':') {
        // Skip drive letter and colon (e.g., "B:")
        snprintf(spiffs_path, sizeof(spiffs_path), "%s%s", BACKGROUND_MOUNT_POINT, path + 2);
    } else {
        // Fallback: assume path is already correct
        strncpy(spiffs_path, path, sizeof(spiffs_path) - 1);
        spiffs_path[sizeof(spiffs_path) - 1] = '\0';
    }
    
    FILE* f = fopen(spiffs_path, flags);
    return (void*)(uintptr_t)f;
}

static lv_fs_res_t fs_close(lv_fs_drv_t* drv, void* file_p) {
    (void)drv;
    FILE* f = (FILE*)(uintptr_t)file_p;
    fclose(f);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t* drv, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    (void)drv;
    FILE* f = (FILE*)(uintptr_t)file_p;
    *br = fread(buf, 1, btr, f);
    return (*br == btr) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t fs_write(lv_fs_drv_t* drv, void* file_p, const void* buf, uint32_t btw, uint32_t* bw) {
    (void)drv;
    FILE* f = (FILE*)(uintptr_t)file_p;
    *bw = fwrite(buf, 1, btw, f);
    return (*bw == btw) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t* drv, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;
    FILE* f = (FILE*)(uintptr_t)file_p;
    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END) w = SEEK_END;
    fseek(f, pos, w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p) {
    (void)drv;
    FILE* f = (FILE*)(uintptr_t)file_p;
    *pos_p = ftell(f);
    return LV_FS_RES_OK;
}

static void* fs_dir_open(lv_fs_drv_t* drv, const char* path) {
    (void)drv;
    // Convert LVGL path to SPIFFS path
    char spiffs_path[128];
    if (path[0] == BACKGROUND_DRIVE_LETTER && path[1] == ':') {
        snprintf(spiffs_path, sizeof(spiffs_path), "%s%s", BACKGROUND_MOUNT_POINT, path + 2);
    } else {
        strncpy(spiffs_path, path, sizeof(spiffs_path) - 1);
        spiffs_path[sizeof(spiffs_path) - 1] = '\0';
    }
    return (void*)opendir(spiffs_path);
}

static lv_fs_res_t fs_dir_read(lv_fs_drv_t* drv, void* dir_p, char* fn, uint32_t fn_len) {
    (void)drv;
    DIR* d = (DIR*)dir_p;
    struct dirent* entry = readdir(d);
    if (entry == NULL) {
        fn[0] = '\0';
        return LV_FS_RES_OK;
    }
    // Copy filename with length limit
    strncpy(fn, entry->d_name, fn_len - 1);
    fn[fn_len - 1] = '\0';
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_dir_close(lv_fs_drv_t* drv, void* dir_p) {
    (void)drv;
    DIR* d = (DIR*)dir_p;
    closedir(d);
    return LV_FS_RES_OK;
}

void LcdDisplay::LoadBackgroundFromSPIFFS() {
    ESP_LOGI(TAG, "Attempting to mount background SPIFFS partition...");
    
    // Mount SPIFFS partition
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BACKGROUND_MOUNT_POINT,
        .partition_label = BACKGROUND_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount background SPIFFS partition");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Background partition not found");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    // Check SPIFFS info
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(BACKGROUND_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Background partition: total %d KB, used %d KB", total / 1024, used / 1024);
    }
    
    // Register LVGL file system driver
    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);
    fs_drv.letter = BACKGROUND_DRIVE_LETTER;
    fs_drv.cache_size = 0;
    fs_drv.open_cb = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb = fs_read;
    fs_drv.write_cb = fs_write;
    fs_drv.seek_cb = fs_seek;
    fs_drv.tell_cb = fs_tell;
    fs_drv.dir_open_cb = fs_dir_open;
    fs_drv.dir_read_cb = fs_dir_read;
    fs_drv.dir_close_cb = fs_dir_close;
    lv_fs_drv_register(&fs_drv);
    
    ESP_LOGI(TAG, "LVGL file system driver registered for background partition (drive: %c:)", BACKGROUND_DRIVE_LETTER);
}
