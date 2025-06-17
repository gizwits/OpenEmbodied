#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class EyeDisplay : public Display {
public:
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
        CONFUSED
    };

    EyeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
              int width, int height, int offset_x, int offset_y,
              bool mirror_x, bool mirror_y,
              DisplayFonts fonts);
    ~EyeDisplay() override;

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

private:
    void SetupUI();
    void StartIdleAnimation();
    void StartHappyAnimation();
    void StartLaughingAnimation();
    void StartSadAnimation();
    void StartAngryAnimation();
    void StartCryingAnimation();
    void StartLovingAnimation();
    void StartEmbarrassedAnimation();
    void StartSurprisedAnimation();
    void StartShockedAnimation();
    void StartThinkingAnimation();
    void StartWinkingAnimation();
    void StartCoolAnimation();
    void StartRelaxedAnimation();
    void StartDeliciousAnimation();
    void StartKissyAnimation();
    void StartConfidentAnimation();
    void StartSleepingAnimation();
    void StartSillyAnimation();
    void StartConfusedAnimation();

    static void EmotionTask(void* arg);
    void ProcessEmotionChange(const char* emotion);

    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    lv_display_t* display_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* zzz1_ = nullptr;  // 第一个 zzz
    lv_obj_t* zzz2_ = nullptr;  // 第二个 zzz
    lv_obj_t* zzz3_ = nullptr;  // 第三个 zzz
    int width_;
    int height_;
    DisplayFonts fonts_;
    EyeState current_state_ = EyeState::IDLE;
    lv_anim_t left_anim_;
    lv_anim_t right_anim_;

    TaskHandle_t emotion_task_ = nullptr;
    QueueHandle_t emotion_queue_ = nullptr;
    static constexpr size_t MAX_EMOTION_LENGTH = 32;
    static constexpr size_t EMOTION_QUEUE_SIZE = 5;
};

#endif // EYE_DISPLAY_H 