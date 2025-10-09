#include "lottie_display.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <cstring>
#include <esp_timer.h>
#include <lvgl.h>
#include <esp_spiffs.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <esp_heap_caps.h>

#define TAG "LottieDisplay"
#define LOTTIE_PARTITION_LABEL "lottie"
#define LOTTIE_MOUNT_POINT "/lottie"

// Define static member variable
static void *animation_buffer = NULL;


LottieDisplay::LottieDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y,
    const lv_img_dsc_t* qrcode_img,
    DisplayFonts fonts)
: LottieDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, fonts)
{
    qrcode_img_ = qrcode_img;
}

LottieDisplay::LottieDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y,
                     DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), qrcode_img_(nullptr), fonts_(fonts) {
    width_ = width;
    height_ = height;

    // Create animation switching queue
    animation_queue_ = xQueueCreate(ANIMATION_QUEUE_SIZE, MAX_ANIMATION_NAME_LENGTH);
    if (animation_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create animation queue");
        return;
    }

    // Create animation switching task with larger stack
    BaseType_t ret = xTaskCreate(
        AnimationTask,
        "animation_task",
        4096 * 6,  // Increased stack size to prevent overflow
        this,
        1,
        &animation_task_
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create animation task");
        vQueueDelete(animation_queue_);
        animation_queue_ = nullptr;
        return;
    }

    InitializeLVGL();

    // Load animations from SPIFFS partition
    LoadAnimationsFromSPIFFS();
    SetupUI();
    
}

LottieDisplay::~LottieDisplay() {
    if (animation_task_ != nullptr) {
        vTaskDelete(animation_task_);
        animation_task_ = nullptr;
    }
    if (animation_queue_ != nullptr) {
        vQueueDelete(animation_queue_);
        animation_queue_ = nullptr;
    }
    // Free loaded animation data or file paths
    for (auto& kv : animations_) {
        if (kv.second.first != nullptr) {
            if (kv.second.second == 0) {
                // It's a file path string
                free((void*)kv.second.first);
            } else {
                // It's memory data (legacy)
                heap_caps_free((void*)kv.second.first);
            }
        }
    }
    animations_.clear();
    
    // Use lock when deleting LVGL objects
    if (current_animation_ != nullptr) {
        lv_obj_del(current_animation_);
        current_animation_ = nullptr;
    }
    if (animation_container_ != nullptr) {
        lv_obj_del(animation_container_);
        animation_container_ = nullptr;
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

void LottieDisplay::InitializeLVGL() {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    
    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 20;
    port_cfg.task_stack = 4096 * 6;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 40),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        // .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .swap_bytes = 1
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    ESP_LOGI(TAG, "LVGL initialized successfully");
}

bool LottieDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LottieDisplay::Unlock() {
    lvgl_port_unlock();
}

void LottieDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr || animation_queue_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    // Copy animation name string to queue
    char* animation_copy = static_cast<char*>(pvPortMalloc(MAX_ANIMATION_NAME_LENGTH));
    if (animation_copy == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for animation name");
        return;
    }

    strncpy(animation_copy, emotion, MAX_ANIMATION_NAME_LENGTH - 1);
    animation_copy[MAX_ANIMATION_NAME_LENGTH - 1] = '\0';

    if (xQueueSend(animation_queue_, &animation_copy, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to send animation to queue");
        vPortFree(animation_copy);
    }
}

void LottieDisplay::AnimationTask(void* arg) {
    LottieDisplay* display = static_cast<LottieDisplay*>(arg);
    char* animation_name = nullptr;

    while (true) {
        if (xQueueReceive(display->animation_queue_, &animation_name, portMAX_DELAY) == pdPASS) {
            if (animation_name != nullptr) {
                display->ProcessAnimationChange(animation_name);
                vPortFree(animation_name);
            }
        }
    }
}



void LottieDisplay::ProcessAnimationChange(const char* animation_name) {
    if (animation_name == nullptr) {
        return;
    }
    
    ESP_LOGI(TAG, "Processing animation change: %s", animation_name);
    
    // Map emotions to available animation files
    // Available files: neutral, happy, sad, angry, thinking
    const char* mapped_animation = "neutral";  // Default animation
    
    if (strcmp(animation_name, "neutral") == 0) {
        mapped_animation = "neutral";
    } else if (strcmp(animation_name, "happy") == 0 || 
               strcmp(animation_name, "cool") == 0 ||
               strcmp(animation_name, "relaxed") == 0 ||
               strcmp(animation_name, "confident") == 0) {
        mapped_animation = "happy";
    } else if (strcmp(animation_name, "laughing") == 0 ||
               strcmp(animation_name, "loving") == 0 ||
               strcmp(animation_name, "kissy") == 0) {
        mapped_animation = "happy";  // Use happy for smile-like emotions
    } else if (strcmp(animation_name, "sad") == 0 ||
               strcmp(animation_name, "crying") == 0) {
        mapped_animation = "sad";
    } else if (strcmp(animation_name, "angry") == 0) {
        mapped_animation = "angry";
    } else if (strcmp(animation_name, "embarrassed") == 0 ||
               strcmp(animation_name, "surprised") == 0 ||
               strcmp(animation_name, "shocked") == 0 ||
               strcmp(animation_name, "delicious") == 0 ||
               strcmp(animation_name, "confused") == 0) {
        mapped_animation = "neutral";  // Use neutral for confused/shocked emotions
    } else if (strcmp(animation_name, "thinking") == 0) {
        mapped_animation = "thinking";  // We have thinking animation
    } else if (strcmp(animation_name, "winking") == 0) {
        mapped_animation = "happy";  // Use happy for winking
    } else if (strcmp(animation_name, "sleepy") == 0) {
        mapped_animation = "neutral";  // Use neutral for sleepy
    } else if (strcmp(animation_name, "silly") == 0) {
        mapped_animation = "happy";  // Use happy for silly
    } else if (strcmp(animation_name, "vertigo") == 0) {
        mapped_animation = "neutral";  // Use neutral for vertigo
    }
    
    ESP_LOGI(TAG, "Mapped emotion '%s' to animation '%s'", animation_name, mapped_animation);
    
    // Use lock to protect state switching
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }
    
#if LV_USE_LOTTIE
    // Build the file path for the animation
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/1/%s.json", LOTTIE_MOUNT_POINT, mapped_animation);
    
    // Check if file exists first
    struct stat st;
    if (stat(filepath, &st) == 0) {
        // Delete the old animation object and create a new one
        if (current_animation_ != nullptr) {
            lv_obj_del(current_animation_);
            current_animation_ = nullptr;
        }
        
        // Create new Lottie animation object
        lv_obj_t * scr = lv_disp_get_scr_act(display_);
        current_animation_ = lv_lottie_create(scr);
        
        // Set up the animation buffer (reuse existing buffer)
        int ANIMATION_WIDTH = (width_ < height_) ? width_ : height_;
        if (animation_buffer != NULL) {
            lv_lottie_set_buffer(current_animation_, ANIMATION_WIDTH, ANIMATION_WIDTH, animation_buffer);
        }
        
        // Center the animation
        lv_obj_center(current_animation_);
        lv_obj_set_size(current_animation_, ANIMATION_WIDTH, ANIMATION_WIDTH);
        
        // Load the new animation file
        lv_lottie_set_src_file(current_animation_, filepath);
        ESP_LOGI(TAG, "Loading animation from file: %s", filepath);
        
        // Ensure the animation is visible
        lv_obj_clear_flag(current_animation_, LV_OBJ_FLAG_HIDDEN);
        
        // Force refresh
        lv_obj_invalidate(current_animation_);
        lv_refr_now(display_);
        
        ESP_LOGI(TAG, "Animation recreated and loaded: %s", filepath);
    } else {
        ESP_LOGW(TAG, "Animation file not found: %s", filepath);
    }
    
#else
    ESP_LOGW(TAG, "Lottie support is not enabled in LVGL configuration");
    ESP_LOGW(TAG, "Enable CONFIG_LV_USE_RLOTTIE=1 and CONFIG_LV_USE_THORVG_INTERNAL=1");
    
    // Fallback: Show a text label
    current_animation_ = lv_label_create(lv_screen_active());
    lv_label_set_text_fmt(current_animation_, "Animation: %s\n(Lottie not enabled)", animation_name);
    lv_obj_center(current_animation_);
#endif
    
    lvgl_port_unlock();
    current_state_ = AnimationState::PLAYING;
    
    ESP_LOGI(TAG, "Animation switched to: %s (mapped: %s)", animation_name, mapped_animation);
}

void LottieDisplay::PlayAnimation(const char* animation_name) {
    SetEmotion(animation_name);
}

void LottieDisplay::StopAnimation() {
    
    if (current_animation_ != nullptr) {
#if LV_USE_LOTTIE
        // Stop the animation using LVGL animation control
        lv_anim_t * anim = lv_lottie_get_anim(current_animation_);
        if (anim) {
            lv_anim_del(anim, NULL);  // Stop the animation
        }
#endif
    }
    
    current_state_ = AnimationState::STOPPED;
    ESP_LOGI(TAG, "Animation stopped");
}


void LottieDisplay::SetupUI() {
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }

    ESP_LOGI(TAG, "SetupUI");
    
    // 对于大的 JSON 文件，需要更大的渲染缓冲区
    // 使用实际显示尺寸作为缓冲区大小（如果内存允许）
    int ANIMATION_WIDTH = (width_ < height_) ? width_ : height_;  // 使用较小的尺寸保持正方形
    if (animation_buffer == NULL) {
        animation_buffer = heap_caps_malloc(ANIMATION_WIDTH * ANIMATION_WIDTH * 4, MALLOC_CAP_SPIRAM);
        if (animation_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate animation buffer from PSRAM");
            return;
        }
        ESP_LOGI(TAG, "Animation buffer allocated in PSRAM: %d bytes at %p", 
                 ANIMATION_WIDTH * ANIMATION_WIDTH * 4, animation_buffer);
    }
    lv_obj_t *scr = lv_disp_get_scr_act(display_);
    
    // 设置屏幕背景为黑色
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 创建 Lottie 动画对象
    lv_obj_t * lottie = lv_lottie_create(scr);

    // 使用 PSRAM 缓冲区
    lv_lottie_set_buffer(lottie, ANIMATION_WIDTH, ANIMATION_WIDTH, animation_buffer);
    ESP_LOGI(TAG, "SetupUI animation_buffer: %p", animation_buffer);
    // 居中显示
    lv_obj_center(lottie);

    // 设置 Lottie 对象大小为缓冲区大小
    lv_obj_set_size(lottie, ANIMATION_WIDTH, ANIMATION_WIDTH);

    ESP_LOGI(TAG, "SetupUI lottie: %p", lottie);
    
    // Store the lottie object for later use
    current_animation_ = lottie;
    
    lvgl_port_unlock();
    
    // Set default animation through SetEmotion
    SetEmotion("neutral");

}

void LottieDisplay::EnterWifiConfig() {
    // ESP_LOGI(TAG, "EnterWifiConfig");
    // if (qrcode_img_) {
    //     ESP_LOGI(TAG, "EnterWifiConfig qrcode_img_ is not null");
    //     auto screen = lv_screen_active();
    //     // Set background to white
    //     lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    //     // Delete all child objects (clear screen)
    //     lv_obj_clean(screen);
    //     // Display QR code image
    //     if (qrcode_img_) {
    //         lv_obj_t* img = lv_img_create(screen);
    //         lv_img_set_src(img, qrcode_img_);
    //         lv_obj_set_style_img_recolor(img, lv_color_hex(0x40E0D0), 0);
    //         lv_obj_center(img);
    //     }
    // }
}

void LottieDisplay::EnterOTAMode() {
    ESP_LOGI(TAG, "EnterOTAMode");
    
    
    // Clear screen
    auto screen = lv_screen_active();
    lv_obj_clean(screen);
    
    // Set black background
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    
    // Create progress arc
    ota_progress_bar_ = lv_arc_create(screen);
    lv_obj_set_size(ota_progress_bar_, height_ - 4, height_ - 4);
    lv_obj_align(ota_progress_bar_, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_value(ota_progress_bar_, 0);
    lv_arc_set_bg_angles(ota_progress_bar_, 0, 360);
    lv_arc_set_rotation(ota_progress_bar_, 270);
    lv_obj_remove_style(ota_progress_bar_, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ota_progress_bar_, LV_OBJ_FLAG_CLICKABLE);
    
    // Set background arc width and color
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_black(), LV_PART_MAIN);
    
    // Set foreground arc width and color
    lv_obj_set_style_arc_width(ota_progress_bar_, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ota_progress_bar_, lv_color_hex(0x40E0D0), LV_PART_INDICATOR);
    
    // Create percentage label
    ota_number_label_ = lv_label_create(screen);
    lv_obj_align(ota_number_label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(ota_number_label_, "0%");
    lv_obj_set_style_text_font(ota_number_label_, fonts_.text_font, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ota_number_label_, lv_color_hex(0x40E0D0), 0);
    
    // Reset progress
    ota_progress_ = 0;
    
    ESP_LOGI(TAG, "OTA mode initialized");
}

void LottieDisplay::LoadAnimationsFromSPIFFS() {
    ESP_LOGI(TAG, "Attempting to mount Lottie SPIFFS partition...");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = LOTTIE_MOUNT_POINT,
        .partition_label = LOTTIE_PARTITION_LABEL,
        .max_files = 10,
        .format_if_mount_failed = false
    };
    
    // Mount SPIFFS partition
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount Lottie SPIFFS partition");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Lottie partition not found");
        } else {
            ESP_LOGW(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    // Check SPIFFS info
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(LOTTIE_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Lottie partition: total %d KB, used %d KB", total / 1024, used / 1024);
    }
    
    // List all JSON files in the partition
    DIR* dir = opendir(LOTTIE_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Failed to open Lottie directory");
        return;
    }
    
    struct dirent* entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        // Check if it's a JSON file
        const char* ext = strrchr(entry->d_name, '.');
        if (ext && strcmp(ext, ".json") == 0) {
            // Remove extension to get animation name
            char name[64];
            size_t name_len = ext - entry->d_name;
            if (name_len >= sizeof(name)) {
                name_len = sizeof(name) - 1;
            }
            strncpy(name, entry->d_name, name_len);
            name[name_len] = '\0';
            
            // Convert to lowercase
            for (char* p = name; *p; p++) {
                *p = tolower(*p);
            }
            
            ESP_LOGI(TAG, "Found animation: %s (%s)", name, entry->d_name);
            
            // Store the file path for later loading
            // We don't load the actual data yet to save memory
            animations_[name] = std::make_pair(nullptr, 0);
            count++;
        }
    }
    closedir(dir);
    
    ESP_LOGI(TAG, "Found %d animations in SPIFFS partition", count);
}

void LottieDisplay::SetOTAProgress(int progress) {
    if (ota_progress_bar_ == nullptr || ota_number_label_ == nullptr) {
        ESP_LOGW(TAG, "OTA mode not initialized");
        return;
    }
    
    // Limit progress range
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;
    
    ota_progress_ = progress;
    
    
    // Update progress bar
    lv_arc_set_value(ota_progress_bar_, progress);
    
    // Update percentage label
    char progress_str[8];
    snprintf(progress_str, sizeof(progress_str), "%d%%", progress);
    lv_label_set_text(ota_number_label_, progress_str);
    
    ESP_LOGI(TAG, "OTA Progress: %d%%", progress);
}