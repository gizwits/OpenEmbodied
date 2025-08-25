#include "toy_data_point_manager.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <functional>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include "wifi_station.h"

#define TAG "ToyDataPointManager"

ToyDataPointManager& ToyDataPointManager::GetInstance() {
    static ToyDataPointManager instance;
    return instance;
}

// 标准实现：获取机智云协议配置
const char* ToyDataPointManager::GetGizwitsProtocolJson() const {
    return R"json(
{
  "name": "标准设备",
  "packetVersion": "0x00000004",
  "protocolType": "var_len",
  "product_key": "standard_product_key",
  "entities": [
    {
      "display_name": "标准设备",
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
          "desc": "设备开关状态"
        },
        {
          "display_name": "唤醒词",
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
          "desc": "唤醒词状态"
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
            "byte_offset": 0,
            "unit": "bit",
            "len": 2,
            "bit_offset": 2
          },
          "type": "status_readonly",
          "id": 2,
          "desc": "充电状态"
        },
        {
          "display_name": "提示音语言",
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
            "bit_offset": 4
          },
          "type": "status_writable",
          "id": 3,
          "desc": "提示音语言"
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
            "bit_offset": 5
          },
          "type": "status_writable",
          "id": 4,
          "desc": "0 按钮\n1 唤醒词\n2 自然对话"
        },
        {
          "display_name": "电量",
          "name": "battery_percentage",
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
          "type": "status_readonly",
          "id": 5,
          "desc": "电池电量百分比"
        },
        {
          "display_name": "音量",
          "name": "volume_set",
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
          "id": 6,
          "desc": "音量设置"
        },
        {
          "display_name": "rssi",
          "name": "rssi",
          "data_type": "uint8",
          "position": {
            "byte_offset": 3,
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
          "id": 7,
          "desc": "WiFi信号强度"
        },
        {
          "display_name": "亮度",
          "name": "brightness",
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
          "id": 8,
          "desc": "屏幕亮度"
        },
        {
            "data_type": "binary",
            "desc": "",
            "display_name": "ssid",
            "id": 9,
            "name": "ssid",
            "position": {
                "bit_offset": 0,
                "byte_offset": 0,
                "len": 100,
                "unit": "byte"
            },
            "type": "status_readonly"
        }
      ],
      "name": "entity0",
      "id": 0
    }
  ]
}
)json";
}

// 标准实现：获取数据点数量
size_t ToyDataPointManager::GetDataPointCount() const {
    return 9; // 9个标准数据点
}

// 标准实现：获取数据点值
bool ToyDataPointManager::GetDataPointValue(const std::string& name, int& value) const {
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
    }
    return false;
}

// 标准实现：设置数据点值
bool ToyDataPointManager::SetDataPointValue(const std::string& name, int value) {
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
    }
    return false;
}

// 标准实现：生成上报数据
void ToyDataPointManager::GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) {
 
    // 固定头部
    buffer[0] = 0x00;
    buffer[1] = 0x00;
    buffer[2] = 0x00;
    buffer[3] = 0x03;
    
    // mqtt 可变长度
    buffer[4] = 0x73;
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
    buffer[13] = 0x03;
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

    // 电量
    if (get_battery_level_callback_) {
        buffer[16] = get_battery_level_callback_();
    } else {
        buffer[16] = 0;
    }

    // 音量
    if (get_volume_callback_) {
        buffer[17] = get_volume_callback_();
    } else {
        buffer[17] = 0;
    }

    // RSSI
    if (get_rssi_callback_) {
        buffer[18] = get_rssi_callback_();
    } else {
        buffer[18] = 0;
    }

    // 亮度
    if (get_brightness_callback_) {
        buffer[19] = get_brightness_callback_();
    } else {
        buffer[19] = 0;
    }

    // 获取 ssid
    std::string ssid = WifiStation::GetInstance().GetSsid();
    if (ssid.length() > 100) {
        ssid = ssid.substr(0, 100);
    }
    
    // 总是复制SSID数据，长度不够100字节的部分用0填充
    if (ssid.length() > 0) {
        memcpy(buffer + 20, ssid.c_str(), ssid.length());
    }
    
    // 用0填充剩余空间到100字节
    if (ssid.length() < 100) {
        memset(buffer + 20 + ssid.length(), 0, 100 - ssid.length());
    }

    data_size = 20 + 100;  // 固定为120字节
    
    ESP_LOGD(TAG, "SSID length: %zu, padded to 100 bytes, total data size: %zu", 
             ssid.length(), data_size);
}

// 标准实现：处理数据点值
void ToyDataPointManager::ProcessDataPointValue(const std::string& name, int value) {
    ESP_LOGI(TAG, "ProcessDataPointValue: %s = %d", name.c_str(), value);
    SetDataPointValue(name, value);
}

void ToyDataPointManager::SetCallbacks(
    std::function<bool()> is_charging_callback,
    std::function<int()> get_chat_mode_callback,
    std::function<void(int)> set_chat_mode_callback,
    std::function<int()> get_battery_level_callback,
    std::function<int()> get_volume_callback,
    std::function<void(int)> set_volume_callback,
    std::function<int()> get_rssi_callback,
    std::function<int()> get_brightness_callback,
    std::function<void(int)> set_brightness_callback
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
}
