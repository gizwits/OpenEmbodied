#include "iot/thing.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <string>


#define TAG "LED"

namespace iot {

// 这里仅定义 LED 的属性和方法，不包含具体的实现
class Led : public Thing {
public:
    Led() : Thing("Led", "A controllable LED light with adjustable brightness") {
        // 定义设备的属性
        properties_.AddNumberProperty("brightness", "Current brightness percentage", [this]() -> int {
            return Board::GetInstance().GetBrightness();
        });

        // 定义设备可以被远程执行的指令
        methods_.AddMethod("set_brightness", "Set the brightness", ParameterList({
            Parameter("brightness", "An integer between 0 and 100", kValueTypeNumber, true)
        }), [this](const ParameterList& parameters) {
            uint8_t brightness = static_cast<uint8_t>(parameters["brightness"].number());
            ESP_LOGI(TAG, "%s Set brightness: %d", name().c_str(), brightness);
            Board::GetInstance().SetBrightness(brightness);
        });
    }

};

} // namespace iot

DECLARE_THING(Led);