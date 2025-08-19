#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "servo.h"

void* create_board();

// 设备工作模式枚举
enum class DeviceMode {
    BUTTON_MODE = 0,      // 按键模式
    WAKE_WORD_MODE = 1,   // 唤醒词模式
    NATURAL_CHAT_MODE = 2 // 自然对话模式
};

class AudioCodec;
class Display;
class BoardCamera;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();
    bool wifi_config_mode_ = false;

    // 软件生成的设备唯一标识
    std::string uuid_;
    
    // 设备工作模式
    DeviceMode device_mode_ = DeviceMode::BUTTON_MODE;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    // 是否要 bo 一下
    virtual bool NeedPlayProcessVoice() { return true; }
    virtual Servo* GetServo();
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual Http* CreateHttp() = 0;
    virtual WebSocket* CreateWebSocket() = 0;
    virtual Mqtt* CreateMqtt() = 0;
    virtual Udp* CreateUdp() = 0;
    virtual void StartNetwork() = 0;
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual uint8_t GetBatteryLevel();
    virtual bool IsCharging();
    virtual std::string GetJson();
    virtual void SetPowerSaveMode(bool enabled) = 0;
    virtual bool IsWifiConfigMode();
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual int MaxVolume() { return 100; }
    virtual uint8_t GetBrightness() { return 0; }
    virtual void SetBrightness(uint8_t brightness) { }
    virtual uint8_t GetDefaultBrightness() { return 0; }
    virtual void EnterDeepSleepIfNotCharging() { }
    
    // 设备模式相关方法
    virtual DeviceMode GetDeviceMode() const { return device_mode_; }
    virtual void SetDeviceMode(DeviceMode mode) { device_mode_ = mode; }
    virtual std::string GetDeviceModeString() const {
        switch (device_mode_) {
            case DeviceMode::BUTTON_MODE:
                return "button_mode";
            case DeviceMode::WAKE_WORD_MODE:
                return "wake_word_mode";
            case DeviceMode::NATURAL_CHAT_MODE:
                return "natural_chat_mode";
            default:
                return "unknown_mode";
        }
    }
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}
#endif // BOARD_H
