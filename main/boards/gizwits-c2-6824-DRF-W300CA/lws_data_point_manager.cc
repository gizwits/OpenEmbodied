#include "lws_data_point_manager.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <functional>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include "wifi_station.h"
#include "settings.h"

#define TAG "LWSDataPointManager"

LWSDataPointManager& LWSDataPointManager::GetInstance() {
    static LWSDataPointManager instance;
    static bool initialized = false;
    if (!initialized) {
        instance.InitFromStorage();
        initialized = true;
    }
    return instance;
}

// 标准实现：获取机智云协议配置
const char* LWSDataPointManager::GetGizwitsProtocolJson() const {
    static const char* protocol_json = R"json(
{
    "name": "小蜜蜂",
    "packetVersion": "0x00000004",
    "protocolType": "standard",
    "product_key": "ca7beafa30214b5fa40559f4fd793284",
    "entities": [
        {
            "display_name": "机智云开发套件",
            "attrs": [
                {
                    "display_name": "开关",
                    "name": "switch",
                    "data_type": "bool",
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "type": "status_writable",
                    "id": 0,
                    "desc": "开关"
                },
                {
                    "display_name": "wakeup_word",
                    "name": "wakeup_word",
                    "data_type": "bool",
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 1
                    },
                    "type": "status_writable",
                    "id": 1,
                    "desc": ""
                },
                {
                    "display_name": "提示音",
                    "name": "alert_tone_language",
                    "data_type": "enum",
                    "enum": [
                        "chinese_simplified",
                        "english"
                    ],
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 2
                    },
                    "type": "status_writable",
                    "id": 2,
                    "desc": ""
                },
                {
                    "display_name": "chat_mode",
                    "name": "chat_mode",
                    "data_type": "enum",
                    "enum": [
                        "0",
                        "1",
                        "2"
                    ],
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 2,
                        "bit_offset": 3
                    },
                    "type": "status_writable",
                    "id": 3,
                    "desc": ""
                },
                {
                    "display_name": "音量",
                    "name": "volume_set",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 1,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 4,
                    "desc": ""
                },
                {
                    "display_name": "brightness",
                    "name": "brightness",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 2,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 5,
                    "desc": ""
                },
                {
                    "display_name": "语速",
                    "name": "speed",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 3,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": -50,
                        "max": 150,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 6,
                    "desc": ""
                },
                {
                    "display_name": "light_speed",
                    "name": "light_speed",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 4,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 7,
                    "desc": ""
                },
                {
                    "display_name": "light_mode",
                    "name": "light_mode",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 5,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 7,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 8,
                    "desc": ""
                },
                {
                    "display_name": "充电状态",
                    "name": "charge_status",
                    "data_type": "enum",
                    "enum": [
                        "none",
                        "charging",
                        "charge_done"
                    ],
                    "position": {
                        "byte_offset": 6,
                        "unit": "bit",
                        "len": 2,
                        "bit_offset": 0
                    },
                    "type": "status_readonly",
                    "id": 9,
                    "desc": ""
                },
                {
                    "display_name": "电量",
                    "name": "battery_percentage",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 7,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_readonly",
                    "id": 10,
                    "desc": ""
                },
                {
                    "display_name": "rssi",
                    "name": "rssi",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 8,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": -100,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_readonly",
                    "id": 11,
                    "desc": ""
                },
                {
                    "display_name": "ssid",
                    "name": "ssid",
                    "data_type": "binary",
                    "position": {
                        "byte_offset": 9,
                        "unit": "byte",
                        "len": 100,
                        "bit_offset": 0
                    },
                    "type": "status_readonly",
                    "id": 12,
                    "desc": ""
                }
            ],
            "name": "entity0",
            "id": 0
        }
    ]
}
)json";
    return protocol_json;
}

// 标准实现：获取数据点数量
size_t LWSDataPointManager::GetDataPointCount() const {
    return 12; // 12个数据点
}

// 标准实现：获取数据点值
bool LWSDataPointManager::GetDataPointValue(const std::string& name, int& value) const {
    if (name == "switch") {
        value = 1; // 开关状态，固定为1
        return true;
    } else if (name == "wakeup_word") {
        value = 1; // 唤醒词状态，固定为1
        return true;
    } else if (name == "charge_status") {
        if (is_charging_callback_) {
            value = is_charging_callback_() ? 1 : 0; // 充电状态
        } else {
            value = 0;
        }
        return true;
    } else if (name == "alert_tone_language") {
        value = 1; // 提示音语言，固定为中文
        return true;
    } else if (name == "chat_mode") {
        if (get_chat_mode_callback_) {
            value = get_chat_mode_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "battery_percentage") {
        if (get_battery_level_callback_) {
            value = get_battery_level_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "volume_set") {
        if (get_volume_callback_) {
            value = get_volume_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "rssi") {
        // RSSI 变化规则：差值大于 20 或者超过 1 分钟才更新
        static int last_rssi_value = 0;
        static auto last_rssi_update_time = std::chrono::steady_clock::now();
        
        if (get_rssi_callback_) {
            int current_rssi = get_rssi_callback_();
            auto current_time = std::chrono::steady_clock::now();
            auto duration_since_last_update = std::chrono::duration_cast<std::chrono::minutes>(current_time - last_rssi_update_time).count();
            
            // 检查是否需要更新 RSSI
            bool should_update = false;
            if (duration_since_last_update >= 1) {
                // 超过 1 分钟，强制更新
                should_update = true;
            } else if (abs(current_rssi - last_rssi_value) > 20) {
                // 差值大于 20，更新
                should_update = true;
            }
            
            if (should_update) {
                last_rssi_value = current_rssi;
                last_rssi_update_time = current_time;
                ESP_LOGD(TAG, "RSSI updated: %d (diff: %d, time: %lld min)", 
                         current_rssi, abs(current_rssi - last_rssi_value), duration_since_last_update);
            }
            
            value = last_rssi_value;
        } else {
            value = 0;
        }
        return true;
    } else if (name == "brightness") {
        if (get_brightness_callback_) {
            value = get_brightness_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "speed") {
        if (get_speed_callback_) {
            value = get_speed_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "light_speed") {
        if (get_light_speed_callback_) {
            value = get_light_speed_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "light_mode") {
        if (get_light_mode_callback_) {
            value = get_light_mode_callback_();
        } else {
            value = 0;
        }
        return true;
    }
    return false;
}

// 标准实现：设置数据点值
bool LWSDataPointManager::SetDataPointValue(const std::string& name, int value) {
    // 写入缓存与存储
    cache_[name] = value;
    // 使用 NVS 进行持久化
    Settings settings("datapoint", true);
    settings.SetInt(name, value);

    if (name == "chat_mode") {
        if (set_chat_mode_callback_) {
            set_chat_mode_callback_(value);
            return true;
        }
    } else if (name == "volume_set") {
        if (set_volume_callback_) {
            set_volume_callback_(value);
            return true;
        }
    } else if (name == "brightness") {
        if (set_brightness_callback_) {
            set_brightness_callback_(value);
            return true;
        }
    } else if (name == "speed") {
        if (set_speed_callback_) {
            set_speed_callback_(value);
            return true;
        }
    } else if (name == "light_speed") {
        if (set_light_speed_callback_) {
            set_light_speed_callback_(value);
            return true;
        }
    } else if (name == "light_mode") {
        if (set_light_mode_callback_) {
            set_light_mode_callback_(value);
            return true;
        }
    }
    return false;
}

// 标准实现：生成上报数据
void LWSDataPointManager::GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) {

    // 固定头部
    buffer[0] = 0x00;
    buffer[1] = 0x00;
    buffer[2] = 0x00;
    buffer[3] = 0x03;
    
    // mqtt 可变长度
    buffer[4] = 0x76;
    // flag
    buffer[5] = 0x00;
    // 命令标识
    buffer[6] = 0x00;
    buffer[7] = 0x93;
    
    // SN
    buffer[8] = 0x00;
    buffer[9] = 0x00;
    buffer[10] = 0x00;
    buffer[11] = 0x02;
    
    // 数据类型
    buffer[12] = 0x14;
    // flag
    buffer[13] = 0x1f;
    buffer[14] = 0xff;

    // 状态字节
    uint8_t status = 0;
    status |= (1 << 0); // switch
    status |= (1 << 1); // wakeup_word
    
    if (is_charging_callback_) {
        status |= (is_charging_callback_() ? 1 : 0) << 2; // charge_status
    }
    
    status |= (1 << 4); // alert_tone_language
    
    if (get_chat_mode_callback_) {
        status |= (get_chat_mode_callback_() << 5); // chat_mode
    }
    
    buffer[15] = status;

    // 音量
    if (get_volume_callback_) {
        buffer[16] = get_volume_callback_();
    } else {
        buffer[16] = 0;
    }

    // 亮度
    if (get_brightness_callback_) {
        buffer[17] = get_brightness_callback_();
    } else {
        buffer[17] = 0;
    }

    // 新增数据点：语速 (byte_offset 5)
    if (get_speed_callback_) {
        buffer[18] = get_speed_callback_();
    } else {
        buffer[18] = 0;
    }

    // 新增数据点：灯光速度 (byte_offset 6)
    if (get_light_speed_callback_) {
        buffer[19] = get_light_speed_callback_();
    } else {
        buffer[19] = 0;
    }

    // 新增数据点：灯光模式 (byte_offset 7)
    if (get_light_mode_callback_) {
        buffer[20] = get_light_mode_callback_();
    } else {
        buffer[20] = 0;
    }

    // 电量
    if (get_battery_level_callback_) {
        buffer[21] = get_battery_level_callback_();
    } else {
        buffer[21] = 0;
    }

    // RSSI
    if (get_rssi_callback_) {
        buffer[22] = get_rssi_callback_();
    } else {
        buffer[22] = 0;
    }

    // 获取 ssid
    std::string ssid = WifiStation::GetInstance().GetSsid();
    if (ssid.length() > 100) {
        ssid = ssid.substr(0, 100);
    }
    
    // 总是复制SSID数据，长度不够100字节的部分用0填充
    if (ssid.length() > 0) {
        memcpy(buffer + 23, ssid.c_str(), ssid.length());
    }
    
    // 用0填充剩余空间到100字节
    if (ssid.length() < 100) {
        memset(buffer + 23 + ssid.length(), 0, 100 - ssid.length());
    }

    data_size = 20 + 100 + 3;  // 固定为123字节
    
    ESP_LOGD(TAG, "SSID length: %zu, padded to 100 bytes, total data size: %zu", 
             ssid.length(), data_size);
}

// 标准实现：处理数据点值
void LWSDataPointManager::ProcessDataPointValue(const std::string& name, int value) {
    ESP_LOGI(TAG, "ProcessDataPointValue: %s = %d", name.c_str(), value);
    SetDataPointValue(name, value);
}

void LWSDataPointManager::SetCallbacks(
    std::function<bool()> is_charging_callback,
    std::function<int()> get_chat_mode_callback,
    std::function<void(int)> set_chat_mode_callback,
    std::function<int()> get_battery_level_callback,
    std::function<int()> get_volume_callback,
    std::function<void(int)> set_volume_callback,
    std::function<int()> get_rssi_callback,
    std::function<int()> get_brightness_callback,
    std::function<void(int)> set_brightness_callback,
    std::function<int()> get_speed_callback,
    std::function<void(int)> set_speed_callback,
    std::function<int()> get_light_speed_callback,
    std::function<void(int)> set_light_speed_callback,
    std::function<int()> get_light_mode_callback,
    std::function<void(int)> set_light_mode_callback
) {
    is_charging_callback_ = is_charging_callback;
    get_chat_mode_callback_ = get_chat_mode_callback;
    set_chat_mode_callback_ = set_chat_mode_callback;
    get_battery_level_callback_ = get_battery_level_callback;
    get_volume_callback_ = get_volume_callback;
    set_volume_callback_ = set_volume_callback;
    get_rssi_callback_ = get_rssi_callback;
    get_brightness_callback_ = get_brightness_callback;
    set_brightness_callback_ = set_brightness_callback;
    get_speed_callback_ = get_speed_callback;
    set_speed_callback_ = set_speed_callback;
    get_light_speed_callback_ = get_light_speed_callback;
    set_light_speed_callback_ = set_light_speed_callback;
    get_light_mode_callback_ = get_light_mode_callback;
    set_light_mode_callback_ = set_light_mode_callback;
}

void LWSDataPointManager::InitFromStorage() {
    // 加载已知可写数据点
    Settings settings("datapoint", false);

    // chat_mode, volume_set, brightness 为可写数据点
    int v;
    v = settings.GetInt("chat_mode", -1);
    if (v != -1) {
        cache_["chat_mode"] = v;
        if (set_chat_mode_callback_) {
            set_chat_mode_callback_(v);
        }
    }

    v = settings.GetInt("volume_set", -1);
    if (v != -1) {
        cache_["volume_set"] = v;
        if (set_volume_callback_) {
            set_volume_callback_(v);
        }
    }

    v = settings.GetInt("brightness", -1);
    if (v != -1) {
        cache_["brightness"] = v;
        if (set_brightness_callback_) {
            set_brightness_callback_(v);
        }
    }

    v = settings.GetInt("speed", -1);
    if (v != -1) {
        cache_["speed"] = v;
        if (set_speed_callback_) {
            set_speed_callback_(v);
        }
    }

    v = settings.GetInt("light_speed", -1);
    if (v != -1) {
        cache_["light_speed"] = v;
        if (set_light_speed_callback_) {
            set_light_speed_callback_(v);
        }
    }

    v = settings.GetInt("light_mode", -1);
    if (v != -1) {
        cache_["light_mode"] = v;
        if (set_light_mode_callback_) {
            set_light_mode_callback_(v);
        }
    }
}

bool LWSDataPointManager::GetCachedDataPoint(const std::string& name, int& value) const {
    auto it = cache_.find(name);
    if (it == cache_.end()) {
        return false;
    }
    value = it->second;
    return true;
}

void LWSDataPointManager::SetCachedDataPoint(const std::string& name, int value) {
    cache_[name] = value;
    Settings settings("datapoint", true);
    settings.SetInt(name, value);
}
