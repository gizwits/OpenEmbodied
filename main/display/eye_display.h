#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H

#include "display.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

class EyeDisplay : public Display {
public:
    enum class EyeState {
        IDLE,       // 待机状态：缓慢眨眼
        THINKING,   // 思考中：快速眨眼
        LISTENING,  // 聆听中：眼睛稍微放大
        SLEEPING,   // 休息中：眼睛半闭
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
    void StartThinkingAnimation();
    void StartListeningAnimation();
    void StartSleepingAnimation();

    esp_lcd_panel_io_handle_t panel_io_;
    esp_lcd_panel_handle_t panel_;
    lv_display_t* display_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    int width_;
    int height_;
    DisplayFonts fonts_;
    EyeState current_state_ = EyeState::IDLE;
    lv_anim_t left_anim_;
    lv_anim_t right_anim_;
};

#endif // EYE_DISPLAY_H 