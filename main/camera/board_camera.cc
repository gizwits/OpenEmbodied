#include "board_camera.h"
#include <esp_log.h>
#include "img_converters.h"
#include "esp_heap_caps.h"
#include <string.h>

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

    // If the frame is not in JPEG format, convert it
    if (fb->format != PIXFORMAT_JPEG) {
        uint8_t* jpeg_buf = nullptr;
        size_t jpeg_size = 0;
        
        // Use lower quality to reduce memory usage
        if (!frame2jpg(fb, 30, &jpeg_buf, &jpeg_size)) {
            lastError = "JPEG conversion failed";
            ESP_LOGE(TAG, "%s", lastError.c_str());
            esp_camera_fb_return(fb);
            return nullptr;
        }

        // Create new frame buffer for JPEG using PSRAM
        camera_fb_t* jpeg_fb = (camera_fb_t*)heap_caps_malloc(sizeof(camera_fb_t), MALLOC_CAP_DEFAULT);
        if (!jpeg_fb) {
            lastError = "Failed to allocate memory for JPEG frame buffer";
            ESP_LOGE(TAG, "%s", lastError.c_str());
            free(jpeg_buf);
            esp_camera_fb_return(fb);
            return nullptr;
        }

        // Allocate JPEG buffer in PSRAM
        uint8_t* psram_buf = (uint8_t*)heap_caps_malloc(jpeg_size, MALLOC_CAP_DEFAULT);
        if (!psram_buf) {
            lastError = "Failed to allocate PSRAM for JPEG data";
            ESP_LOGE(TAG, "%s", lastError.c_str());
            heap_caps_free(jpeg_fb);
            free(jpeg_buf);
            esp_camera_fb_return(fb);
            return nullptr;
        }

        // Copy JPEG data to PSRAM
        memcpy(psram_buf, jpeg_buf, jpeg_size);
        free(jpeg_buf);  // Free the original buffer immediately

        jpeg_fb->buf = psram_buf;
        jpeg_fb->len = jpeg_size;
        jpeg_fb->width = fb->width;
        jpeg_fb->height = fb->height;
        jpeg_fb->format = PIXFORMAT_JPEG;
        jpeg_fb->timestamp = fb->timestamp;

        // Free original frame buffer immediately
        esp_camera_fb_return(fb);
        return jpeg_fb;
    }

    return fb;
}

std::string BoardCamera::getLastError() const {
    return lastError;
}
