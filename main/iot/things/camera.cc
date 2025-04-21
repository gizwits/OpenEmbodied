#include "iot/thing.h"
#include "board.h"
#include "settings.h"
#include "camera/board_camera.h"

#include <esp_log.h>
#include <string>

#define TAG "Camera"

namespace iot {

// 这里仅定义 Camera 的属性和方法，不包含具体的实现
class Camera : public Thing {
public:
    Camera() : Thing("Camera", "相机") {
        methods_.AddMethod("takePhoto", "拍照", ParameterList({
            Parameter("theme_name", "主题模式, light 或 dark", kValueTypeString, true)
        }), [this](const ParameterList& parameters) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                camera_fb_t* fb = camera->capture();
                if (fb) {
                    esp_camera_fb_return(fb); // 释放帧缓冲区
                } else {
                    ESP_LOGE(TAG, "Failed to capture image: %s", camera->getLastError().c_str());
                }
            }
        });
    }
};

} // namespace iot

DECLARE_THING(Camera);
