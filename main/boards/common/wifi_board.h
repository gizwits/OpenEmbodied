#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    void EnterWifiConfigMode();
    virtual std::string GetBoardJson() override;
    void CheckTmpFactoryTestMode();
    void CheckTmpFactoryTestModeWithWifiConfig();

public:
    WifiBoard();
    virtual NetworkType GetNetworkType() override { return NetworkType::WIFI; }
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();
    virtual bool IsWifiConfigMode() override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    virtual void SetBrightness(uint8_t brightness) override;
    virtual uint8_t GetDefaultBrightness() override;
    virtual void EnterDeepSleepIfNotCharging() override;
};

#endif // WIFI_BOARD_H
