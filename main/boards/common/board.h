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
#include <network_interface.h>

// 前向声明
class PowerSaveTimer;
//enum NetworkType
enum class NetworkType {
    WIFI,
    ML307
};


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
    
public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual NetworkType GetNetworkType() { return NetworkType::WIFI; }

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
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) { return false; }
    virtual std::string GetJson();
    virtual void SetPowerSaveMode(bool enabled) = 0;
    virtual int GetDefaultChatMode() { return 1; }
    virtual bool IsWifiConfigMode();
    
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual int MaxVolume() { return 100; }
    virtual int MaxBacklightBrightness() { return 100; }
    virtual int GetVoiceSpeed() { return 0; }
    virtual bool IsCharging() { return false; }
    virtual void PowerOff() {};
    virtual void ResetPowerSaveTimer() {};  // 新增：重置电源保存定时器
    virtual void WakeUpPowerSaveTimer() {};
    virtual PowerSaveTimer* GetPowerSaveTimer() { return nullptr; }  // 新增：获取电源保存定时器
    virtual uint8_t GetBrightness() { return 0; }
    virtual void SetBrightness(uint8_t brightness) { }
    virtual uint8_t GetDefaultBrightness() { return 0; }
    virtual void EnterDeepSleepIfNotCharging() { }
    virtual bool GetNeedPlayPrologue() { return false; }
    virtual bool NeedPlayProcessVoiceWithLife() { return false; }
    // 充电状态是否静默启动
    virtual bool NeedSilentStartup() { return false; }
    virtual bool NeedBlockLowBattery() { return false; }
    
    // 数据点相关方法
    virtual const char* GetGizwitsProtocolJson() const { return nullptr; }
    virtual size_t GetDataPointCount() const { return 0; }
    virtual bool GetDataPointValue(const std::string& name, int& value) const { return false; }
    virtual bool SetDataPointValue(const std::string& name, int value) { return false; }
    virtual void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) { data_size = 0; }
    virtual void ProcessDataPointValue(const std::string& name, int value) {}

};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}
#endif // BOARD_H
