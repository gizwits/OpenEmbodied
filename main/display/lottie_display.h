#ifndef LOTTIE_DISPLAY_H
#define LOTTIE_DISPLAY_H

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <map>
#include <string>

#define DISPLAY_VERTICAL_OFFSET 20

class LottieDisplay : public Display {
public:
    enum class AnimationState {
        IDLE,
        PLAYING,
        STOPPED
    };

    LottieDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
              int width, int height, int offset_x, int offset_y,
              bool mirror_x, bool mirror_y,
              DisplayFonts fonts);
    LottieDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                int width, int height, int offset_x, int offset_y,
                bool mirror_x, bool mirror_y,
                const lv_img_dsc_t* qrcode_img,
                DisplayFonts fonts);
    ~LottieDisplay() override;

    bool Lock(int timeout_ms) override;
    void Unlock() override;

    // Empty implementations for unused interfaces
    virtual void SetStatus(const char* status) override {}
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {}
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override {}
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override {}
    virtual void SetIcon(const char* icon) override {}
    virtual void SetPreviewImage(const lv_img_dsc_t* image) override {}
    virtual void SetTheme(const std::string& theme_name) override {}
    virtual void UpdateStatusBar(bool update_all = false) override {}
    virtual void EnterWifiConfig() override;
    virtual void EnterOTAMode() override;
    virtual void SetOTAProgress(int progress) override;

    // Play Lottie animation
    void PlayAnimation(const char* animation_name);
    void StopAnimation();
    
    // Load animations from SPIFFS
    void LoadAnimationsFromSPIFFS();

private:
    void SetupUI();
    void InitializeLVGL();
    
    static void AnimationTask(void* arg);
    void ProcessAnimationChange(const char* animation_name);

    int ota_progress_ = 0;
    lv_obj_t* ota_progress_bar_ = nullptr;
    lv_obj_t* ota_number_label_ = nullptr;

    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    lv_display_t* display_ = nullptr;
    
    lv_obj_t* animation_container_ = nullptr;
    lv_obj_t* current_animation_ = nullptr;


    const lv_img_dsc_t* qrcode_img_ = nullptr;

    int width_;
    int height_;
    DisplayFonts fonts_;
    AnimationState current_state_ = AnimationState::IDLE;
    
    bool animation_loop_ = true;
    
    TaskHandle_t animation_task_ = nullptr;
    QueueHandle_t animation_queue_ = nullptr;
    StackType_t* task_stack_ = nullptr;  // PSRAM allocated stack
    StaticTask_t* task_buffer_ = nullptr;  // Internal RAM allocated task buffer
    static constexpr size_t MAX_ANIMATION_NAME_LENGTH = 64;
    static constexpr size_t ANIMATION_QUEUE_SIZE = 5;
    
    // Animation data storage
    std::map<std::string, std::pair<const uint8_t*, size_t>> animations_;
};

#endif // LOTTIE_DISPLAY_H