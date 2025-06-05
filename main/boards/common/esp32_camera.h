#ifndef ESP32_CAMERA_H
#define ESP32_CAMERA_H

#include <esp_camera.h>
#include <lvgl.h>
#include <thread>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    camera_fb_t* fb_ = nullptr;
    lv_img_dsc_t preview_image_;
    std::string explain_url_="https://api.coze.cn/v1/files/upload";
    std::string explain_token_="pat_qhYD0r6M87zqCNLUOvImYB6vC7cVPTQn6rmVO1FIVzjpppHmoOEeam4nlxr9ykAO";
    std::thread encoder_thread_;

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    virtual std::string Explain(const std::string& question);
    virtual std::string Explain_kouzi(const std::string& question);
};

#endif // ESP32_CAMERA_H