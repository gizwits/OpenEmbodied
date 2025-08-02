#ifndef XUNGUAN_DISPLAY_H
#define XUNGUAN_DISPLAY_H

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>
#include <driver/spi_master.h>
#include <esp_lcd_gc9a01.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include "assert.h"

class XunguanDisplay : public Display {
public:
    // Empty enum for compatibility
    enum class EyeState {
        IDLE,       // 待机状态：缓慢眨眼
        HAPPY,
        LAUGHING,
        SAD,
        ANGRY,
        CRYING,
        LOVING,
        EMBARRASSED,
        SURPRISED,
        SHOCKED,
        THINKING,
        WINKING,
        COOL,
        RELAXED,
        DELICIOUS,
        KISSY,
        CONFIDENT,
        SLEEPING,
        SILLY,
        VERTIGO,
        CONFUSED
    };

    // Animation queue system
    enum class AnimationType {
        NONE,
        IDLE,
        HAPPY,
        SAD,
        LOVING,
        THINKING,
        SHOCKED,
        SLEEPING,
        SILLY,
        VERTIGO
    };
    
    struct AnimationRequest {
        AnimationType type;
        bool is_pending;
        AnimationRequest() : type(AnimationType::NONE), is_pending(false) {}
    };
    
    AnimationRequest pending_animation_;
    bool animation_queue_enabled_;
    
    // Animation queue methods
    void QueueAnimation(AnimationType type);
    void ProcessAnimationQueue();
    void StopCurrentAnimation();
    void StartAnimation(AnimationType type);

    // Frame rate control interface
    enum class FrameRateMode {
        POWER_SAVE = 0,    // 低功耗模式：8-20ms (50-125Hz)
        NORMAL = 1,        // 正常模式：5-15ms (67-200Hz) 
        SMOOTH = 2,        // 流畅模式：2-8ms (125-500Hz)
        CUSTOM = 3         // 自定义模式
    };
    
    // Frame rate control methods
    bool SetFrameRateMode(FrameRateMode mode);
    bool SetCustomFrameRate(uint32_t min_ms, uint32_t max_ms);
    FrameRateMode GetCurrentFrameRateMode() const { return current_frame_rate_mode_; }
    uint32_t GetCurrentMinDelay() const { return current_min_delay_ms_; }
    uint32_t GetCurrentMaxDelay() const { return current_max_delay_ms_; }
    uint32_t GetCurrentTickPeriod() const { return current_tick_period_us_; }
    

private:
    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    lv_display_t* lvgl_display_;
    bool initialized_;
    
    // Frame rate control variables
    FrameRateMode current_frame_rate_mode_;
    uint32_t current_min_delay_ms_;
    uint32_t current_max_delay_ms_;
    uint32_t current_tick_period_us_;
    
    // Frame rate control methods
    bool UpdateFrameRateSettings(FrameRateMode mode, uint32_t min_ms, uint32_t max_ms, uint32_t tick_period_us);
    void ApplyFrameRateSettings();
    
    // UI elements - simplified
    lv_obj_t* left_eye_;
    lv_obj_t* container_;
    
    // Sleep UI elements
    lv_obj_t* zzz1_;  // First "z" label
    lv_obj_t* zzz2_;  // Second "z" label  
    lv_obj_t* zzz3_;  // Third "z" label
    lv_anim_t zzz1_anim_;  // Animation for first "z"
    lv_anim_t zzz2_anim_;  // Animation for second "z"
    lv_anim_t zzz3_anim_;  // Animation for third "z"
    
    // Current state
    EyeState current_state_;
    
    // Animation variables
    lv_anim_t left_eye_anim_;
    lv_anim_t right_eye_anim_;
    lv_obj_t* right_eye_;
    
    // Mouth animation variables
    lv_obj_t* mouth_;
    lv_anim_t mouth_anim_;
    
    // LVGL flush callback
    static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    
    // Display update callback
    static void lvgl_port_update_callback(lv_display_t* disp);
    
    // LVGL task
    static void lvgl_task(void* arg);
    
    // Mutex for LVGL thread safety
    static _lock_t lvgl_api_lock;
    
    // Timer for LVGL tick
    esp_timer_handle_t lvgl_tick_timer_;
    
    // Task handle
    TaskHandle_t lvgl_task_handle_;
    
    // Vertigo recovery timer
    esp_timer_handle_t vertigo_recovery_timer_;
    bool vertigo_mode_active_;

    // Loving recovery timer
    esp_timer_handle_t loving_recovery_timer_;
    bool loving_mode_active_;

    // OTA mode variables
    lv_obj_t* ota_progress_bar_;
    lv_obj_t* ota_number_label_;
    int ota_progress_;

    // Blink animation user data structure
    struct BlinkUserData {
        lv_obj_t* right_eye;
        int original_left_y;
        int original_right_y;
    };

public:
    XunguanDisplay();
    ~XunguanDisplay() override;
    
    // Initialize display hardware
    bool Initialize();
    
    // Override Display methods
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    
    // Setup UI
    void SetupUI();
    
    // Set emotion (override from Display) - empty for compatibility
    void SetEmotion(const char* emotion) override;
    
    // Test function to cycle through emotions - empty for compatibility
    void TestNextEmotion();
    
    // Animation functions - empty for compatibility
    void start_blink_animation();
    void start_look_animation();
    void start_idle_animation();
    void start_scale_animation();
    
    // Animation methods for different states
    void StartIdleAnimation();
    void StartHappyAnimation();
    void StartSadAnimation();
    void StartLovingAnimation();
    void StartThinkingAnimation();
    void StartShockedAnimation();
    void StartSleepingAnimation();
    void StartSillyAnimation();
    void StartVertigoAnimation();

    // OTA mode methods
    void EnterOTAMode();
    void SetOTAProgress(int progress);

    // WiFi config method
    void EnterWifiConfig();

private:
    // Initialize SPI bus
    bool InitializeSpi();
    
    // Initialize LCD panel
    bool InitializeLcdPanel();
    
    // Initialize LVGL
    bool InitializeLvgl();
    
    // Initialize LVGL timer
    bool InitializeLvglTimer();
    
    // Create LVGL task
    bool CreateLvglTask();
    
    // Helper function to clear existing UI elements
    void ClearUIElements();
    
    // Animation helper functions
    void StartEyeScalingAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye, int original_height);
    void StartSimpleColorAnimation(lv_obj_t* obj);  // Text opacity animation
    void StartHeartScalingAnimation(lv_obj_t* left_heart, lv_obj_t* right_heart, int original_width, int original_height);
    void StartSillyEyeHeightAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye);  // Silly eye height animation
    void StartVertigoRotationAnimation(lv_obj_t* left_spiral, lv_obj_t* right_spiral);  // Vertigo rotation animation
    void StartHappyBlinkingAnimation(lv_obj_t* left_circle, lv_obj_t* right_circle, int original_size);  // Happy blinking animation
    void StartTearFallingAnimation(lv_obj_t* left_tear, lv_obj_t* right_tear, int start_y);  // Tear falling animation
    void StartThinkingFloatAnimation(lv_obj_t* left_eye, lv_obj_t* right_eye, int original_y);  // Thinking float animation
    static void simple_color_anim_cb(void* var, int32_t v);
    static void heart_zoom_anim_cb(void* var, int32_t v);
    static void blink_anim_cb(void* var, int32_t v);  // Blinking animation callback
    static void thinking_float_anim_cb(void* var, int32_t v);  // Thinking float animation callback
    static void eye_scaling_anim_cb(void* var, int32_t v);  // Synchronized eye scaling animation callback
};

#endif // XUNGUAN_DISPLAY_H 