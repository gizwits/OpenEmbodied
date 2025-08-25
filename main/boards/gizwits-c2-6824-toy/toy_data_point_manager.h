#pragma once

#include "data_point_manager.h"
#include <string>
#include <cstdint>
#include <cstddef>
#include <functional>

class ToyDataPointManager : public DataPointManager {
public:
    static ToyDataPointManager& GetInstance();
    
    // 获取机智云协议配置
    virtual const char* GetGizwitsProtocolJson() const;
    
    // 获取数据点数量
    virtual size_t GetDataPointCount() const;
    
    // 获取数据点值
    virtual bool GetDataPointValue(const std::string& name, int& value) const;
    
    // 设置数据点值
    virtual bool SetDataPointValue(const std::string& name, int value);
    
    // 生成上报数据
    virtual void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size);
    
    // 处理数据点值
    virtual void ProcessDataPointValue(const std::string& name, int value);
    
    // 设置依赖的回调函数
    void SetCallbacks(
        std::function<bool()> is_charging_callback,
        std::function<int()> get_chat_mode_callback,
        std::function<void(int)> set_chat_mode_callback,
        std::function<int()> get_battery_level_callback,
        std::function<int()> get_volume_callback,
        std::function<void(int)> set_volume_callback,
        std::function<int()> get_rssi_callback,
        std::function<int()> get_brightness_callback,
        std::function<void(int)> set_brightness_callback
    );

protected:
    ToyDataPointManager() = default;
    virtual ~ToyDataPointManager() = default;
    ToyDataPointManager(const ToyDataPointManager&) = delete;
    ToyDataPointManager& operator=(const ToyDataPointManager&) = delete;
    
    // 回调函数
    std::function<bool()> is_charging_callback_;
    std::function<int()> get_chat_mode_callback_;
    std::function<void(int)> set_chat_mode_callback_;
    std::function<int()> get_battery_level_callback_;
    std::function<int()> get_volume_callback_;
    std::function<void(int)> set_volume_callback_;
    std::function<int()> get_rssi_callback_;
    std::function<int()> get_brightness_callback_;
    std::function<void(int)> set_brightness_callback_;
};
