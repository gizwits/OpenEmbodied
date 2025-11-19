#include "eye_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "board.h"
#include "font_awesome_symbols.h"
// 包含板级配置文件以访问 WiFi 图标
#include "config.h"
#include "w25q64_flash.h"

LV_FONT_DECLARE(font_awesome_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

#define EYE_COLOR 0x40E0D0  // Tiffany Blue color for eyes

#define TAG "EyeDisplay"

// 表情名称到视频组索引的映射表（共享，避免重复定义）
static const struct {
    const char* name;
    int group_index;
} emotion_group_map[] = {
    {"happy",        0},  // 组 0开心表情
    {"neutral",      1},  // 组 1中性开机表情
    {"sad",          2},  // 组 2悲伤表情
    {"surprised",    3},  // 组 3惊讶表情
    {"angry",        4},  // 组 4愤怒表情
    {"loving",       5},  // 组 5喜爱表情
    {"thinking",     6},  // 组 6思考表情
    {"winking",      7},  // 组 7眨眼表情
    {"sleepy",       8},  // 组 8睡觉表情
    // {"silly",        9},  // 组 9愚蠢表情       
    {"vertigo",     9},  // 组 10眩晕表情
    {"listen",      10},  // 组 11聆听表情
    {"Turn_right",  11},  // 组 13右转表情
    {"Turn_left",   12},  // 组 12左转表情
    {"Accelerate",  13},  // 组 14加速表情
    {"Decelerate",  14},  // 组 15急刹表情
    {"Charging",    15}   // 组 16充电表情
};

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
    if (battery_update_timer_ != nullptr) {
        esp_timer_stop(battery_update_timer_);
        esp_timer_delete(battery_update_timer_);
        battery_update_timer_ = nullptr;
    }
}

bool EyeDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EyeDisplay::Unlock() {
    lvgl_port_unlock();
}

void EyeDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        ESP_LOGW(TAG, "SetEmotion: emotion is nullptr");
        return;
    }
    
    // 如果正在切换模式，延迟执行或跳过（避免在模式切换期间触发显示更新）
    __sync_synchronize();
    if (mode_switching_) {
        ESP_LOGD(TAG, "SetEmotion: Mode switching, skipping emotion change");
        return;
    }
    __sync_synchronize();
    
    // 更新显示模式为固定表情模式
    current_display_mode_ = DisplayMode::FIXED_EMOTION;
    
    // 查找表情对应的组索引（使用共享的映射表）
    int group_index = -1;
    for (size_t i = 0; i < sizeof(emotion_group_map) / sizeof(emotion_group_map[0]); i++) {
        if (strcmp(emotion, emotion_group_map[i].name) == 0) {
            group_index = emotion_group_map[i].group_index;
            break;
        }
    }
    
    if (group_index < 0) {
        ESP_LOGW(TAG, "SetEmotion: unknown emotion '%s', using default neutral (group 1)", emotion);
        group_index = 1;  // 默认使用 neutral
    }
    
    ESP_LOGI(TAG, "SetEmotion: '%s' -> group %d", emotion, group_index);
    
    // 播放对应的视频组
    PlayVideoGroup(group_index);
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

    // 启动时自动播放 index = 1 的表情（neutral）
    // 使用延迟任务确保 Flash 已经初始化
    video_group_index_ = 1;
    current_display_mode_ = DisplayMode::FIXED_EMOTION;
    
    // 延迟启动视频播放，给 Flash 初始化一些时间
    // 如果 Flash 未初始化，视频播放任务会自己处理并退出
    ESP_LOGI(TAG, "SetupUI: Will start video playback with group index=1 after delay");
    xTaskCreate([](void* arg) {
        vTaskDelay(pdMS_TO_TICKS(500));  // 延迟 500ms，确保 Flash 已初始化
        auto* self = static_cast<EyeDisplay*>(arg);
        self->StartVideoPlayback();
        vTaskDelete(nullptr);
    }, "start_video", 4096, this, 5, nullptr);
    
    // 创建电量显示UI（默认隐藏，层级最高）
    CreateBatteryIndicator();
}

void EyeDisplay::TestNextEmotion() {
   
} 

void EyeDisplay::EnterWifiConfig() {
    ESP_LOGI(TAG, "EnterWifiConfig");
    
    // 更新显示模式
    current_display_mode_ = DisplayMode::WIFI_CONFIG;
    
    // 禁用表情切换
    emotion_disabled_ = true;
    
    if (qrcode_img_) {
        ESP_LOGI(TAG, "EnterWifiConfig qrcode_img_ is not null");
        DisplayLockGuard lock(this);
        auto screen = lv_screen_active();
        if (screen == nullptr) {
            ESP_LOGE(TAG, "EnterWifiConfig: screen is nullptr");
            return;
        }
        
        // 先停止视频播放任务，避免访问已删除的对象
        StopVideoPlayback();
        
        // 清空 video_img_ 指针，因为对象将被删除
        video_img_ = nullptr;
        
        // 先逐个删除子对象，避免访问已删除的对象
        // 注意：删除对象前先检查对象是否有效，避免访问已删除的对象
        // 保留充电环，如果它正在显示
        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
        ESP_LOGI(TAG, "EnterWifiConfig: 删除 %u 个子对象（保留充电环）", child_cnt);
        for (int32_t i = child_cnt - 1; i >= 0; i--) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (child != nullptr && lv_obj_is_valid(child)) {
                // 跳过充电环和充电标签，如果它们正在显示
                if (battery_indicator_showing_ && 
                    (child == battery_arc_ || child == battery_label_)) {
                    continue;
                }
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
            lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_center(img);
            // 确保二维码图片在最前面（但充电环应该在更前面）
            lv_obj_move_foreground(img);
            // 如果充电环正在显示，确保它在最前面
            if (battery_indicator_showing_ && battery_arc_ != nullptr && lv_obj_is_valid(battery_arc_)) {
                lv_obj_move_foreground(battery_arc_);
            }
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
    
    // 更新显示模式
    current_display_mode_ = DisplayMode::OTA_MODE;
    
    // 禁用表情切换
    emotion_disabled_ = true;
    
    // 隐藏充电环，避免显示冲突
    if (battery_indicator_showing_) {
        HiddenBatteryLevel();
    }
    
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
    
    // 确保充电环保持隐藏（如果之前显示了）
    if (battery_indicator_showing_ && battery_arc_ != nullptr && lv_obj_is_valid(battery_arc_)) {
        lv_obj_add_flag(battery_arc_, LV_OBJ_FLAG_HIDDEN);
    }
    
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

void EyeDisplay::ShowWifiSignalAndBattery() {
    ESP_LOGI(TAG, "ShowWifiSignalAndBattery: 显示Wi-Fi信号和电量");
    
    // 查找当前视频组对应的表情名称（使用共享的映射表）
    const char* current_emotion = "neutral";  // 默认值
    for (size_t i = 0; i < sizeof(emotion_group_map) / sizeof(emotion_group_map[0]); i++) {
        if (emotion_group_map[i].group_index == video_group_index_) {
            current_emotion = emotion_group_map[i].name;
            break;
        }
    }
    
    // 保存当前状态
    saved_emotion_before_battery_ = current_emotion;
    was_video_mode_before_battery_ = (current_display_mode_ == DisplayMode::VIDEO_CYCLING);
    saved_video_group_index_ = video_group_index_;
    ESP_LOGI(TAG, "保存当前状态: emotion=%s, video_mode=%d, group_index=%d", 
             saved_emotion_before_battery_.c_str(), was_video_mode_before_battery_, saved_video_group_index_);
    
    // 更新显示模式
    current_display_mode_ = DisplayMode::BATTERY_SIGNAL;
    
    // 隐藏视频图像
    if (video_img_ != nullptr && lv_obj_is_valid(video_img_)) {
        DisplayLockGuard lock(this);
        lv_obj_add_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 显示电量（复用 ShowBatteryLevel）
    ShowBatteryLevel();
    
    // 在圆环中心显示Wi-Fi信号图标
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGW(TAG, "Screen is nullptr");
        return;
    }
    
    // 如果已经存在信号图标，先删除
    if (signal_img_ != nullptr) {
        if (lv_obj_is_valid(signal_img_)) {
            lv_obj_del(signal_img_);
        }
        signal_img_ = nullptr;
    }
    
    // 获取当前网络状态图标
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
            lv_img_set_src(signal_img_, wifi_img);
            lv_obj_set_size(signal_img_, wifi_img->header.w, wifi_img->header.h);
            lv_obj_align(signal_img_, LV_ALIGN_CENTER, 0, 0);  // 居中显示在圆环中心
            lv_obj_clear_flag(signal_img_, LV_OBJ_FLAG_HIDDEN);
            // 将图标颜色改为主题色
            lv_obj_set_style_img_recolor(signal_img_, lv_color_hex(EYE_COLOR), 0);
            lv_obj_set_style_img_recolor_opa(signal_img_, LV_OPA_COVER, 0);
            lv_obj_invalidate(signal_img_);
            ESP_LOGI(TAG, "信号图片已设置: size=%dx%d", wifi_img->header.w, wifi_img->header.h);
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
    if (signal_img_ != nullptr) {
        lv_obj_move_foreground(signal_img_);
    }
    
    // 确保充电环也在最前面
    if (battery_arc_ != nullptr && lv_obj_is_valid(battery_arc_)) {
        lv_obj_move_foreground(battery_arc_);
    }
    
    ESP_LOGI(TAG, "Wi-Fi信号和电量已显示");
    
    // 创建定时器，5秒后自动隐藏并恢复表情
    if (battery_display_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                // 使用 Application::Schedule 将恢复操作调度到主应用线程执行
                // arg 是 EyeDisplay* 指针
                EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                Application::GetInstance().Schedule([self]() {
                    self->RestoreStateAfterBattery();
                }, "RestoreStateAfterBattery_Timer");
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_display_timer"
        };
        esp_timer_create(&timer_args, &battery_display_timer_);
    }
    esp_timer_stop(battery_display_timer_);
    esp_timer_start_once(battery_display_timer_, 5000000);  // 5秒后恢复
}

void EyeDisplay::RestoreStateAfterBattery() {
    ESP_LOGI(TAG, "RestoreStateAfterBattery: 恢复之前的状态");
    
    // 检查是否正在充电
    int level = 0;
    bool charging = false;
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    
    // 如果正在充电，不隐藏充电环；否则隐藏
    if (!charging) {
        HiddenBatteryLevel();
    } else {
        ESP_LOGI(TAG, "正在充电，保持充电环显示");
    }
    
    DisplayLockGuard lock(this);
    
    // 删除信号图标
    if (signal_img_ != nullptr) {
        if (lv_obj_is_valid(signal_img_)) {
            lv_obj_del(signal_img_);
        }
        signal_img_ = nullptr;
    }
    
    // 恢复之前的显示模式
    if (was_video_mode_before_battery_) {
        // 恢复轮播模式
        current_display_mode_ = DisplayMode::VIDEO_CYCLING;
        video_group_index_ = saved_video_group_index_;
        ESP_LOGI(TAG, "恢复轮播模式，group_index=%d", video_group_index_);
    } else {
        // 恢复固定表情模式
        current_display_mode_ = DisplayMode::FIXED_EMOTION;
        if (!saved_emotion_before_battery_.empty()) {
            ESP_LOGI(TAG, "恢复固定表情模式，emotion=%s", saved_emotion_before_battery_.c_str());
            SetEmotion(saved_emotion_before_battery_.c_str());
        } else {
            // 如果没有保存的表情，使用保存的组索引
            video_group_index_ = saved_video_group_index_;
            ESP_LOGI(TAG, "恢复固定表情模式，group_index=%d", video_group_index_);
        }
    }
    
    // 恢复视频图像显示
    if (video_img_ != nullptr && lv_obj_is_valid(video_img_)) {
        lv_obj_clear_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "状态已恢复");
}

void EyeDisplay::CreateBatteryIndicator() {
    DisplayLockGuard lock(this);
    
    auto screen = lv_screen_active();
    if (screen == nullptr) {
        ESP_LOGE(TAG, "CreateBatteryIndicator: screen is nullptr");
        return;
    }
    
    // 创建电量圆环
    battery_arc_ = lv_arc_create(screen);
    lv_obj_set_size(battery_arc_, height_ - 4, height_ - 4);
    lv_obj_align(battery_arc_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_value(battery_arc_, 0);
    lv_arc_set_bg_angles(battery_arc_, 0, 360);
    lv_arc_set_rotation(battery_arc_, 270);  // 从顶部开始
    lv_obj_remove_style(battery_arc_, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(battery_arc_, LV_OBJ_FLAG_CLICKABLE);
    
    // 设置背景弧宽度和颜色（再缩小一半，从7改为3）
    lv_obj_set_style_arc_width(battery_arc_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(battery_arc_, lv_color_black(), LV_PART_MAIN);
    
    // 设置前景弧宽度（颜色会根据电量动态设置，再缩小一半）
    lv_obj_set_style_arc_width(battery_arc_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(battery_arc_, lv_color_hex(0x00FF00), LV_PART_INDICATOR);  // 默认绿色
    
    // 创建百分比标签（但不显示）
    battery_label_ = lv_label_create(screen);
    lv_obj_align(battery_label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(battery_label_, "0%");
    lv_obj_set_style_text_font(battery_label_, fonts_.text_font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x00FF00), 0);
    
    // 默认隐藏，层级最高（标签始终隐藏）
    lv_obj_add_flag(battery_arc_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(battery_arc_);
    // 标签不需要移到前景，因为始终隐藏
    
    // 创建定时器用于自动更新电量
    if (battery_update_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                EyeDisplay* self = static_cast<EyeDisplay*>(arg);
                if (!self->battery_indicator_showing_) return;
                
                int level = 0;
                bool charging = false;
                bool discharging = false;
                if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
                    self->UpdateBatteryLevel(level);
                }
            },
            .arg = this,
            .name = "battery_update"
        };
        esp_timer_create(&timer_args, &battery_update_timer_);
    }
    
    ESP_LOGI(TAG, "Battery indicator UI created");
}

void EyeDisplay::UpdateBatteryLevel(int level) {
    if (battery_arc_ == nullptr || battery_label_ == nullptr) {
        return;
    }
    
    // 如果正在切换模式，跳过更新（避免在切换过程中触发内存分配）
    // 使用内存屏障确保读取到最新的标志位值
    __sync_synchronize();
    if (mode_switching_) {
        ESP_LOGD(TAG, "UpdateBatteryLevel: Mode switching, skipping update");
        return;
    }
    __sync_synchronize();
    
    // 限制电量范围
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    
    // 尝试获取锁，如果失败则跳过更新（避免阻塞）
    if (!Lock(50)) {
        ESP_LOGW(TAG, "UpdateBatteryLevel: Failed to acquire lock, skipping update");
        return;
    }
    
    // 检查对象是否有效
    if (!lv_obj_is_valid(battery_arc_)) {
        Unlock();
        return;
    }
    
    // 根据电量设置颜色：<20%红色，<50%黄色，否则绿色
    uint32_t color;
    if (level < 20) {
        color = 0xFF0000;  // 红色
    } else if (level < 50) {
        color = 0xFFFF00;  // 黄色
    } else {
        color = 0x00FF00;  // 绿色
    }
    
    // 更新进度条（在锁保护下操作，避免并发问题）
    // 注意：如果内存不足，LVGL可能会失败，但不会崩溃（由LVGL内部处理）
    lv_arc_set_value(battery_arc_, level);
    lv_obj_set_style_arc_color(battery_arc_, lv_color_hex(color), LV_PART_INDICATOR);
    
    // 确保充电环始终在最前面（这个操作可能触发重绘和内存分配）
    // 如果内存不足，LVGL会返回失败，但不会崩溃
    lv_obj_move_foreground(battery_arc_);
    
    Unlock();
    
    // 不更新百分比标签（中间不显示数字）
}

void EyeDisplay::ShowBatteryLevel() {
    if (battery_arc_ == nullptr || battery_label_ == nullptr) {
        ESP_LOGW(TAG, "Battery indicator not created, creating now");
        CreateBatteryIndicator();
    }
    
    DisplayLockGuard lock(this);

    ESP_LOGI(TAG, "ShowBatteryLevel");
    
    // 显示电量UI（只显示圆环，不显示数字）
    lv_obj_clear_flag(battery_arc_, LV_OBJ_FLAG_HIDDEN);
    // battery_label_ 保持隐藏，不显示数字
    
    // 确保在最前面
    lv_obj_move_foreground(battery_arc_);
    
    // 立即更新一次电量
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        UpdateBatteryLevel(level);
    }
    
    // 启动定时器，每2秒更新一次
    if (battery_update_timer_ != nullptr) {
        esp_timer_stop(battery_update_timer_);
        esp_timer_start_periodic(battery_update_timer_, 2000000);  // 2秒
    }
    
    battery_indicator_showing_ = true;
    ESP_LOGI(TAG, "Battery indicator shown");
}

void EyeDisplay::HiddenBatteryLevel() {
    DisplayLockGuard lock(this);
    
    // 隐藏电量UI
    if (battery_arc_ != nullptr) {
        lv_obj_add_flag(battery_arc_, LV_OBJ_FLAG_HIDDEN);
    }
    if (battery_label_ != nullptr) {
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // 停止定时器
    if (battery_update_timer_ != nullptr) {
        esp_timer_stop(battery_update_timer_);
    }
    
    battery_indicator_showing_ = false;
    ESP_LOGI(TAG, "Battery indicator hidden");
}

// 视频播放任务（持续运行，监听 index 变化并自动切换视频组）
void EyeDisplay::VideoPlayTask(void* arg) {
    auto* self = static_cast<EyeDisplay*>(arg);
    auto& flash = W25Q64Flash::GetInstance();
    
    if (!flash.IsInitialized()) {
        ESP_LOGE(TAG, "Flash not initialized");
        self->video_playing_ = false;
        self->video_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    
    // Read header: 1 byte count + N*4 bytes frame counts
    uint8_t group_count = 0;
    esp_err_t read_ret = flash.Read(EyeDisplay::kVideoFlashBaseAddress, &group_count, 1);
    if (read_ret != ESP_OK) {
        // 如果Flash被锁定，等待解锁
        if (flash.IsLocked()) {
            ESP_LOGI(TAG, "Flash is locked for erase/write, waiting for unlock...");
            // 等待Flash解锁（最多等待5分钟）
            for (int i = 0; i < 300 && flash.IsLocked(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒检查一次
            }
            if (flash.IsLocked()) {
                ESP_LOGE(TAG, "Flash still locked after 5 minutes, exiting video task");
                self->video_playing_ = false;
                self->video_task_handle_ = nullptr;
                vTaskDelete(nullptr);
                return;
            }
            // 重新读取
            read_ret = flash.Read(EyeDisplay::kVideoFlashBaseAddress, &group_count, 1);
        }
        if (read_ret != ESP_OK || group_count == 0) {
            ESP_LOGE(TAG, "invalid video header");
            self->video_playing_ = false;
            self->video_task_handle_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
    }
    
    std::vector<uint32_t> frame_counts(group_count, 0);
    read_ret = flash.Read(EyeDisplay::kVideoFlashBaseAddress + 1, (uint8_t*)frame_counts.data(), group_count * sizeof(uint32_t));
    if (read_ret != ESP_OK) {
        // 如果Flash被锁定，等待解锁
        if (flash.IsLocked()) {
            ESP_LOGI(TAG, "Flash is locked for erase/write, waiting for unlock...");
            // 等待Flash解锁（最多等待5分钟）
            for (int i = 0; i < 300 && flash.IsLocked(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒检查一次
            }
            if (flash.IsLocked()) {
                ESP_LOGE(TAG, "Flash still locked after 5 minutes, exiting video task");
                self->video_playing_ = false;
                self->video_task_handle_ = nullptr;
                vTaskDelete(nullptr);
                return;
            }
            // 重新读取
            read_ret = flash.Read(EyeDisplay::kVideoFlashBaseAddress + 1, (uint8_t*)frame_counts.data(), group_count * sizeof(uint32_t));
        }
        if (read_ret != ESP_OK) {
            ESP_LOGE(TAG, "read frame counts failed");
            self->video_playing_ = false;
            self->video_task_handle_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
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
    
    // Allocate frame buffer (reused for all groups)
    uint8_t* buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)malloc(frame_size);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for frame buffer");
        self->video_playing_ = false;
        self->video_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    
    // Create video image object once
    if (self->Lock(50)) {
        if (self->video_img_ == nullptr) {
            lv_obj_t* screen = lv_screen_active();
            if (screen != nullptr) {
                self->video_img_ = lv_image_create(screen);
                lv_obj_set_size(self->video_img_, self->width_, self->height_);
                lv_obj_set_pos(self->video_img_, 0, 0);
                lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(self->video_img_, LV_OBJ_FLAG_FLOATING);
                // 确保对象可见
                lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
                // 如果电量环未显示，才把视频移到最前面
                if (!self->battery_indicator_showing_) {
                    lv_obj_move_foreground(self->video_img_);
                }
                ESP_LOGI(TAG, "Video image object created, img=%p", self->video_img_);
            } else {
                ESP_LOGE(TAG, "Failed to get active screen for video image");
            }
        }
        self->Unlock();
    }
    
    // Current playing state
    int current_group = -1;
    uint32_t current_frames = 0;
    uint32_t current_idx = 0;
    uint32_t last_idx = UINT32_MAX;  // 记录上一帧的索引，用于检测循环完成
    
    // Main loop: continuously play video, switch group when index changes
    while (self->video_playing_) {
        // 检查当前显示模式
        DisplayMode current_mode = self->current_display_mode_;
        
        // 如果是特殊模式（OTA、配网、电量信号），停止播放并等待模式切换
        if (current_mode == DisplayMode::OTA_MODE || 
            current_mode == DisplayMode::WIFI_CONFIG || 
            current_mode == DisplayMode::BATTERY_SIGNAL) {
            // 检查视频图像对象是否有效（可能已被删除）
            if (self->Lock(20)) {
                if (self->video_img_ != nullptr && lv_obj_is_valid(self->video_img_)) {
                    lv_obj_add_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
                } else if (self->video_img_ != nullptr) {
                    // 对象已被删除，清空指针
                    self->video_img_ = nullptr;
                }
                self->Unlock();
            }
            // 等待模式切换
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // 确保视频图像可见（如果之前被隐藏了）
        if (self->Lock(20)) {
            if (self->video_img_ != nullptr && lv_obj_is_valid(self->video_img_)) {
                lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
            } else if (self->video_img_ != nullptr) {
                // 对象已被删除，清空指针，下次循环会重新创建
                self->video_img_ = nullptr;
            }
            self->Unlock();
        }
        
        // Check if group index has changed
        int target_group = self->video_group_index_;
        if (target_group < 0 || target_group >= group_count) {
            target_group = 0;
        }
        
        // If group changed, switch to new group
        if (target_group != current_group) {
            current_group = target_group;
            current_frames = frame_counts[current_group];
            current_idx = 0;
            last_idx = UINT32_MAX;  // 重置，避免误判
            
            if (current_frames == 0) {
                ESP_LOGE(TAG, "No frames in group %d, skipping", current_group);
                vTaskDelay(pdMS_TO_TICKS(100)); // Wait a bit before checking again
                continue;
            }
            
            ESP_LOGI(TAG, "Switching to video group %d, frames=%u, mode=%d", 
                     current_group, (unsigned)current_frames, (int)current_mode);
            
            // Read first frame of new group
            size_t off0 = EyeDisplay::kVideoFlashBaseAddress + group_base[current_group] + current_idx * frame_size;
            esp_err_t read_ret = flash.Read(off0, buf, frame_size);
            if (read_ret != ESP_OK) {
                // 如果Flash被锁定（正在擦写），等待并降低日志级别
                if (flash.IsLocked()) {
                    ESP_LOGD(TAG, "Flash is locked for erase/write, waiting...");
                    vTaskDelay(pdMS_TO_TICKS(1000));  // 等待1秒
                    continue;
                }
                ESP_LOGE(TAG, "read first frame of group %d failed", current_group);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            
            // Display first frame
            if (self->Lock(20)) {
                if (self->video_img_ != nullptr && lv_obj_is_valid(self->video_img_)) {
                    self->video_img_dsc_.header.w = self->width_;
                    self->video_img_dsc_.header.h = self->height_;
                    self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
                    self->video_img_dsc_.data = buf;
                    self->video_img_dsc_.data_size = frame_size;
                    lv_img_set_src(self->video_img_, &self->video_img_dsc_);
                    lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
                    // 如果电量环未显示，才把视频移到最前面
                    if (!self->battery_indicator_showing_) {
                        lv_obj_move_foreground(self->video_img_);
                    }
                    // 强制刷新
                    lv_obj_invalidate(self->video_img_);
                } else if (self->video_img_ != nullptr) {
                    // 对象已被删除，清空指针，下次循环会重新创建
                    self->video_img_ = nullptr;
                }
                self->Unlock();
            }
            
            current_idx = (current_idx + 1) % current_frames;
            vTaskDelay(pdMS_TO_TICKS(self->kVideoFrameDelayMs));
        }
        
        // Play current frame
        if (current_frames > 0) {
            size_t off = EyeDisplay::kVideoFlashBaseAddress + group_base[current_group] + current_idx * frame_size;
            esp_err_t read_ret = flash.Read(off, buf, frame_size);
            if (read_ret != ESP_OK) {
                // 如果Flash被锁定（正在擦写），等待并降低日志级别
                if (flash.IsLocked()) {
                    ESP_LOGD(TAG, "Flash is locked for erase/write, waiting...");
                    vTaskDelay(pdMS_TO_TICKS(1000));  // 等待1秒
                    continue;
                }
                ESP_LOGE(TAG, "read frame %u of group %d failed", (unsigned)current_idx, current_group);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            
            // Update LVGL image
            if (self->Lock(20)) {
                if (self->video_img_ != nullptr && lv_obj_is_valid(self->video_img_)) {
                    self->video_img_dsc_.header.w = self->width_;
                    self->video_img_dsc_.header.h = self->height_;
                    self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
                    self->video_img_dsc_.data = buf;
                    self->video_img_dsc_.data_size = frame_size;
                    lv_img_set_src(self->video_img_, &self->video_img_dsc_);
                    // 确保对象可见
                    lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
                    // 如果电量环未显示，才把视频移到最前面
                    if (!self->battery_indicator_showing_) {
                        lv_obj_move_foreground(self->video_img_);
                    }
                } else if (self->video_img_ != nullptr) {
                    // 对象已被删除，清空指针，下次循环会重新创建
                    self->video_img_ = nullptr;
                }
                self->Unlock();
            }
            
            // if ((current_idx % 10) == 0) {
            //     ESP_LOGI(TAG, "Playing group=%d idx=%u/%u mode=%d", 
            //              current_group, (unsigned)current_idx, (unsigned)current_frames, (int)current_mode);
            // }
            
            // 记录当前帧索引，然后更新到下一帧
            last_idx = current_idx;
            current_idx = (current_idx + 1) % current_frames;
            
            // 检查是否播放完当前组（从最后一帧循环回第一帧时）
            // 条件：current_idx 变成 0，且上一帧是最后一帧（说明完成了一轮播放）
            bool group_completed = (current_idx == 0 && last_idx != UINT32_MAX && 
                                    last_idx == current_frames - 1 && current_frames > 1);
            // 特殊情况：如果只有一帧，播放一次就算完成
            if (current_frames == 1 && last_idx == 0) {
                group_completed = true;
            }
            
            // 如果完成了一轮播放，根据模式决定下一步动作
            if (group_completed) {
                // ESP_LOGI(TAG, "Group %d finished one cycle, mode=%d", current_group, (int)current_mode);
                
                // 根据模式决定下一步动作
                if (current_mode == DisplayMode::VIDEO_CYCLING) {
                    // 轮播模式：检查是否锁定
                    if (self->cycling_locked_) {
                        // 锁定状态：循环播放当前组（不切换到下一个组）
                        // ESP_LOGI(TAG, "VIDEO_CYCLING (locked): Group %d finished, looping same group", current_group);
                        last_idx = UINT32_MAX;  // 重置，避免重复判断
                    } else {
                        // 未锁定：自动切换到下一个组
                        int next_group = (current_group + 1) % group_count;
                        // ESP_LOGI(TAG, "VIDEO_CYCLING: Group %d finished, cycling to next group %d", current_group, next_group);
                        self->video_group_index_ = next_group;
                        // 重置状态，下一轮循环会切换到新组
                        current_group = -1;  // 强制触发组切换
                        last_idx = UINT32_MAX;  // 重置
                    }
                } else if (current_mode == DisplayMode::FIXED_EMOTION) {
                    // 固定表情模式：循环播放当前组（current_idx 已经是 0，会继续播放）
                    // ESP_LOGI(TAG, "FIXED_EMOTION: Group %d finished, looping same group", current_group);
                    // current_idx 已经是 0，会继续循环播放
                    last_idx = UINT32_MAX;  // 重置，避免重复判断
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(self->kVideoFrameDelayMs));
    }
    
    // Cleanup
    free(buf);
    self->video_playing_ = false;
    self->video_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void EyeDisplay::StartVideoPlayback() {
    // If task is already running, just return
    if (video_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Video playback task already running");
        return;
    }
    
    ESP_LOGI(TAG, "StartVideoPlayback, initial group=%d", video_group_index_);
    video_playing_ = true;
    // Lower priority from 5 to 1, avoid blocking audio task (audio task usually priority 3-4)
    xTaskCreate(VideoPlayTask, "video_play", 4096, this, 1, &video_task_handle_);
}

void EyeDisplay::ToggleVideoCyclingMode() {
    ESP_LOGI(TAG, "ToggleVideoCyclingMode");
    
    // 设置切换标志，防止在切换过程中更新电量（使用内存屏障确保可见性）
    mode_switching_ = true;
    __sync_synchronize();  // 内存屏障，确保标志位对所有CPU核心可见
    
    // 在切换模式前，先停止电量更新定时器，避免在切换过程中触发更新
    if (battery_update_timer_ != nullptr && battery_indicator_showing_) {
        esp_timer_stop(battery_update_timer_);
        // 等待定时器完全停止（给定时器任务时间处理停止请求）
        vTaskDelay(pdMS_TO_TICKS(100));  // 增加等待时间，确保定时器完全停止
    }
    
    if (current_display_mode_ == DisplayMode::VIDEO_CYCLING) {
        current_display_mode_ = DisplayMode::FIXED_EMOTION;
        // 直接设置视频组索引并播放，避免调用SetEmotion()触发额外的显示更新
        video_group_index_ = 8;  // sleepy表情对应组8
        PlayVideoGroup(8);
        // 解锁轮播锁定
        cycling_locked_ = false;
    } else {
        current_display_mode_ = DisplayMode::VIDEO_CYCLING;
        // 切换到轮播模式时，解锁锁定状态
        cycling_locked_ = false;
    }
    
    // 确保视频播放任务正在运行
    if (video_task_handle_ == nullptr) {
        StartVideoPlayback();
    }
    
    // 等待一段时间，确保所有切换操作完成，LVGL绘制完成
    vTaskDelay(pdMS_TO_TICKS(150));  // 增加等待时间，确保LVGL绘制完成
    
    // 切换完成，清除标志（使用内存屏障）
    __sync_synchronize();
    mode_switching_ = false;
    __sync_synchronize();
    
    // 如果电量指示器正在显示，延迟重新启动定时器（再等一段时间确保稳定）
    if (battery_update_timer_ != nullptr && battery_indicator_showing_) {
        vTaskDelay(pdMS_TO_TICKS(150));  // 增加等待时间
        esp_timer_start_periodic(battery_update_timer_, 2000000);  // 2秒
    }
}

void EyeDisplay::ToggleCyclingLock() {
    cycling_locked_ = !cycling_locked_;
    ESP_LOGI(TAG, "ToggleCyclingLock: %s", cycling_locked_ ? "锁定当前视频" : "继续轮播");
}

void EyeDisplay::StopVideoPlayback() {
    if (!video_playing_ && video_task_handle_ == nullptr) return;
    video_playing_ = false;
    // Wait for task to self-delete and clean up handle
    for (int i = 0; i < 50 && video_task_handle_ != nullptr; ++i) { // Wait up to 500ms
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    video_task_handle_ = nullptr;
}

void EyeDisplay::PlayVideoGroup(int index) {
    ESP_LOGI(TAG, "PlayVideoGroup called with index=%d", index);
    
    auto& flash = W25Q64Flash::GetInstance();
    if (!flash.IsInitialized()) {
        ESP_LOGE(TAG, "Flash not initialized");
        return;
    }
    
    // Validate index by reading flash
    uint8_t group_count = 0;
    if (flash.Read(EyeDisplay::kVideoFlashBaseAddress, &group_count, 1) != ESP_OK || group_count == 0) {
        ESP_LOGE(TAG, "invalid video header or no groups");
        return;
    }
    
    if (index < 0 || index >= group_count) {
        ESP_LOGE(TAG, "Invalid video group index %d, valid range: 0-%d", index, group_count - 1);
        return;
    }
    
    // Simply update the group index - the task will detect the change and switch
    video_group_index_ = index;
    ESP_LOGI(TAG, "Video group index updated to %d, task will switch automatically", index);
    
    // Ensure video playback task is running
    if (video_task_handle_ == nullptr) {
        StartVideoPlayback();
    }
}

