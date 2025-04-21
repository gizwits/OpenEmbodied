#include "board_camera.h"
#include <esp_log.h>

#define TAG "Camera"

BoardCamera::BoardCamera() : initialized(false) {
}

BoardCamera::~BoardCamera() {
    if (initialized) {
        esp_camera_deinit();
    }
}

bool BoardCamera::init(const camera_config_t& config) {
    // Initialize the camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        lastError = "Camera initialization failed with error: " + std::to_string(err);
        ESP_LOGE(TAG, "%s", lastError.c_str());
        return false;
    }

    initialized = true;
    return true;
}

camera_fb_t* BoardCamera::capture() {
    if (!initialized) {
        lastError = "Camera not initialized";
        ESP_LOGE(TAG, "%s", lastError.c_str());
        return nullptr;
    }

    // Capture a frame
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        lastError = "Failed to capture image";
        ESP_LOGE(TAG, "%s", lastError.c_str());
        return nullptr;
    }

    return fb;
}

std::string BoardCamera::getLastError() const {
    return lastError;
}
