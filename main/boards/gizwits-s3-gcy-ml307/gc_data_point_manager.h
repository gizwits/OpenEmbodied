#pragma once

#include <string>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>

class GCDataPointManager {
public:
    static GCDataPointManager& GetInstance();
    
    // 获取机智云协议配置
    virtual const char* GetGizwitsProtocolJson() const;
    
    // 获取数据点数量
    virtual size_t GetDataPointCount() const;
    
    // 获取数据点值
    virtual bool GetDataPointValue(const std::string& name, uint32_t& value) const;
    
    // 设置数据点值
    virtual bool SetDataPointValue(const std::string& name, uint32_t value);
    
    // 生成上报数据
    virtual void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size);
    
    // 处理数据点值
    virtual void ProcessDataPointValue(const std::string& name, uint32_t value);
    
    // 处理二进制数据点值
    virtual void ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len);
    
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
        std::function<void(int)> set_brightness_callback,
        std::function<void(const std::string&)> img_bg_callback,
        std::function<void(const std::string&)> video_bg_callback,
        // 新增数据点的回调函数
        std::function<bool()> get_switch_callback,
        std::function<void(bool)> set_switch_callback,
        std::function<bool()> get_wakeup_word_callback,
        std::function<void(bool)> set_wakeup_word_callback,
        std::function<int()> get_alert_tone_language_callback,
        std::function<void(int)> set_alert_tone_language_callback,
        std::function<int()> get_speed_callback,
        std::function<void(int)> set_speed_callback,
        std::function<uint32_t(int)> get_timer_callback,  // 参数为timer索引(1-10)
        std::function<void(int, uint32_t)> set_timer_callback,  // 参数为timer索引(1-10)和值
        std::function<void(int, const std::string&)> set_tts_callback,  // 参数为tts索引(1-10)和文本
        std::function<int()> get_wake_word_index_callback,  // 获取唤醒词索引
        std::function<void(int)> set_wake_word_index_callback  // 设置唤醒词索引
    );

    // 初始化：从存储加载并应用缓存的数据点
    void InitFromStorage();

    // 查询缓存数据点（如果不存在则返回false）
    bool GetCachedDataPoint(const std::string& name, int& value) const;

    // 设置缓存但不触发回调（用于内部恢复时调用回调前的缓存同步）
    void SetCachedDataPoint(const std::string& name, int value);
    
    // 获取当前唤醒词索引
    int GetCurrentWakeWordIndex() const { return current_wake_word_index_; }

protected:
    GCDataPointManager() = default;
    virtual ~GCDataPointManager() = default;
    GCDataPointManager(const GCDataPointManager&) = delete;
    GCDataPointManager& operator=(const GCDataPointManager&) = delete;
    
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
    std::function<void(const std::string&)> img_bg_callback_;
    std::function<void(const std::string&)> video_bg_callback_;
    // 新增数据点的回调函数
    std::function<bool()> get_switch_callback_;
    std::function<void(bool)> set_switch_callback_;
    std::function<bool()> get_wakeup_word_callback_;
    std::function<void(bool)> set_wakeup_word_callback_;
    std::function<int()> get_alert_tone_language_callback_;
    std::function<void(int)> set_alert_tone_language_callback_;
    std::function<int()> get_speed_callback_;
    std::function<void(int)> set_speed_callback_;
    std::function<uint32_t(int)> get_timer_callback_;  // 参数为timer索引(1-10)
    std::function<void(int, uint32_t)> set_timer_callback_;  // 参数为timer索引(1-10)和值
    std::function<void(int, const std::string&)> set_tts_callback_;  // 参数为tts索引(1-10)和文本
    std::function<int()> get_wake_word_index_callback_;  // 获取唤醒词索引
    std::function<void(int)> set_wake_word_index_callback_;  // 设置唤醒词索引

    // 简单的内存缓存
    std::map<std::string, int> cache_;
    
    // 当前唤醒词索引
    int current_wake_word_index_ = 0;
};
