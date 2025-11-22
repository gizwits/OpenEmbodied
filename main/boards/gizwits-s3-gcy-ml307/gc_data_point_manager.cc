#include "gc_data_point_manager.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <functional>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>
#include <cstdint>
#include "wifi_station.h"
#include "settings.h"

#define TAG "GCDataPointManager"

GCDataPointManager& GCDataPointManager::GetInstance() {
    static GCDataPointManager instance;
    static bool initialized = false;
    if (!initialized) {
        instance.InitFromStorage();
        initialized = true;
    }
    return instance;
}

// 标准实现：获取机智云协议配置
const char* GCDataPointManager::GetGizwitsProtocolJson() const {
    static const char* protocol_json = R"json(
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
            "desc": "1"
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
            "desc": ""
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
            "desc": "0 按钮\n1 唤醒词\n2 自然对话"
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
            "display_name": "亮度",
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
            "desc": "输出音频的语速，取值范围 [-50, 100]，默认为 0。-50 表示 0.5 倍速，100 表示 2 倍速。"
        },
        {
            "display_name": "闹钟1",
            "name": "timer1",
            "data_type": "uint32",
            "position": {
                "byte_offset": 4,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 7,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟2",
            "name": "timer2",
            "data_type": "uint32",
            "position": {
                "byte_offset": 8,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 8,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟3",
            "name": "timer3",
            "data_type": "uint32",
            "position": {
                "byte_offset": 12,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 9,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟4",
            "name": "timer4",
            "data_type": "uint32",
            "position": {
                "byte_offset": 16,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 10,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟5",
            "name": "timer5",
            "data_type": "uint32",
            "position": {
                "byte_offset": 20,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 11,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟6",
            "name": "timer6",
            "data_type": "uint32",
            "position": {
                "byte_offset": 24,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 12,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟7",
            "name": "timer7",
            "data_type": "uint32",
            "position": {
                "byte_offset": 28,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 13,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟8",
            "name": "timer8",
            "data_type": "uint32",
            "position": {
                "byte_offset": 32,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 14,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟9",
            "name": "timer9",
            "data_type": "uint32",
            "position": {
                "byte_offset": 36,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 15,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "闹钟10",
            "name": "timer10",
            "data_type": "uint32",
            "position": {
                "byte_offset": 40,
                "unit": "byte",
                "len": 4,
                "bit_offset": 0
            },
            "uint_spec": {
                "addition": 0,
                "max": 4294967295,
                "ratio": 1,
                "min": 0
            },
            "type": "status_writable",
            "id": 16,
            "desc": "时间点的时间戳"
        },
        {
            "display_name": "bg_img",
            "name": "bg_img",
            "data_type": "binary",
            "position": {
                "byte_offset": 44,
                "unit": "byte",
                "len": 255,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 17,
            "desc": ""
        },
        {
            "display_name": "bg_video",
            "name": "bg_video",
            "data_type": "binary",
            "position": {
                "byte_offset": 299,
                "unit": "byte",
                "len": 255,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 18,
            "desc": ""
        },
        {
            "display_name": "闹钟1的文字提示",
            "name": "tts1",
            "data_type": "binary",
            "position": {
                "byte_offset": 554,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 19,
            "desc": ""
        },
        {
            "display_name": "闹钟2的文字提示",
            "name": "tts2",
            "data_type": "binary",
            "position": {
                "byte_offset": 614,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 20,
            "desc": ""
        },
        {
            "display_name": "闹钟3的文字提示",
            "name": "tts3",
            "data_type": "binary",
            "position": {
                "byte_offset": 674,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 21,
            "desc": ""
        },
        {
            "display_name": "闹钟4的文字提示",
            "name": "tts4",
            "data_type": "binary",
            "position": {
                "byte_offset": 734,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 22,
            "desc": ""
        },
        {
            "display_name": "闹钟5的文字提示",
            "name": "tts5",
            "data_type": "binary",
            "position": {
                "byte_offset": 794,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 23,
            "desc": ""
        },
        {
            "display_name": "闹钟6的文字提示",
            "name": "tts6",
            "data_type": "binary",
            "position": {
                "byte_offset": 854,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 24,
            "desc": ""
        },
        {
            "display_name": "闹钟7的文字提示",
            "name": "tts7",
            "data_type": "binary",
            "position": {
                "byte_offset": 914,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 25,
            "desc": ""
        },
        {
            "display_name": "闹钟8的文字提示",
            "name": "tts8",
            "data_type": "binary",
            "position": {
                "byte_offset": 974,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 26,
            "desc": ""
        },
        {
            "display_name": "闹钟9的文字提示",
            "name": "tts9",
            "data_type": "binary",
            "position": {
                "byte_offset": 1034,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 27,
            "desc": ""
        },
        {
            "display_name": "闹钟10的文字提示",
            "name": "tts10",
            "data_type": "binary",
            "position": {
                "byte_offset": 1094,
                "unit": "byte",
                "len": 60,
                "bit_offset": 0
            },
            "type": "status_writable",
            "id": 28,
            "desc": ""
        },
        {
            "display_name": "充电状态",
            "name": "charge_status",
            "data_type": "enum",
            "enum": [
                "none",
                " charging",
                "charge_done"
            ],
            "position": {
                "byte_offset": 1154,
                "unit": "bit",
                "len": 2,
                "bit_offset": 0
            },
            "type": "status_readonly",
            "id": 29,
            "desc": ""
        },
        {
            "display_name": "电量",
            "name": "battery_percentage",
            "data_type": "uint8",
            "position": {
                "byte_offset": 1155,
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
            "id": 30,
            "desc": ""
        },
        {
            "display_name": "rssi",
            "name": "rssi",
            "data_type": "uint8",
            "position": {
                "byte_offset": 1156,
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
            "id": 31,
            "desc": "无"
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
size_t GCDataPointManager::GetDataPointCount() const {
    return 9; // 9个标准数据点
}

// 标准实现：获取数据点值
bool GCDataPointManager::GetDataPointValue(const std::string& name, uint32_t& value) const {
    if (name == "switch") {
        if (get_switch_callback_) {
            value = get_switch_callback_() ? 1 : 0;
        } else {
            value = 1; // 默认值
        }
        return true;
    } else if (name == "wakeup_word") {
        if (get_wakeup_word_callback_) {
            value = get_wakeup_word_callback_() ? 1 : 0;
        } else {
            value = 1; // 默认值
        }
        return true;
    } else if (name == "charge_status") {
        if (is_charging_callback_) {
            value = is_charging_callback_() ? 1 : 0; // 充电状态
        } else {
            value = 0;
        }
        return true;
    } else if (name == "alert_tone_language") {
        if (get_alert_tone_language_callback_) {
            value = get_alert_tone_language_callback_();
        } else {
            value = 1; // 默认值：中文
        }
        return true;
    } else if (name == "chat_mode") {
        if (get_chat_mode_callback_) {
            value = get_chat_mode_callback_();
        } else {
            value = 0;
        }
        return true;
    } else if (name == "speed") {
        if (get_speed_callback_) {
            value = get_speed_callback_();
        } else {
            value = 0; // 默认值
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
    } else if (name.find("timer") == 0 && name.length() > 5) {
        // 处理 timer1-timer10
        std::string timer_num_str = name.substr(5); // 提取数字部分
        int timer_index = std::atoi(timer_num_str.c_str());
        if (timer_index >= 1 && timer_index <= 10 && get_timer_callback_) {
            value = get_timer_callback_(timer_index);
            // 如果值超过 int32_t 最大值，尝试从字符串读取
            if (value > static_cast<uint32_t>(INT32_MAX)) {
                Settings settings("datapoint", false);
                std::string str_value = settings.GetString(name, "");
                if (!str_value.empty()) {
                    try {
                        value = std::stoul(str_value);
                    } catch (const std::out_of_range& oor) {
                        ESP_LOGE(TAG, "Timer %s string to uint32_t conversion out of range: %s", name.c_str(), oor.what());
                    } catch (const std::invalid_argument& ia) {
                        ESP_LOGE(TAG, "Timer %s string to uint32_t conversion invalid argument: %s", name.c_str(), ia.what());
                    }
                }
            }
            return true;
        }
        return false;
    }
    return false;
}

// 标准实现：设置数据点值
bool GCDataPointManager::SetDataPointValue(const std::string& name, uint32_t value) {
    // 写入缓存（如果值在 int 范围内）
    if (value <= static_cast<uint32_t>(INT32_MAX)) {
        cache_[name] = static_cast<int>(value);
    } else {
        // 超过 int32_t 最大值，缓存中存储 -1 作为标记
        cache_[name] = -1;
    }
    
    // 使用 NVS 进行持久化
    Settings settings("datapoint", true);
    
    // 如果值超过 int32_t 最大值，使用字符串存储
    if (value > static_cast<uint32_t>(INT32_MAX)) {
        settings.SetString(name, std::to_string(value));
        ESP_LOGI(TAG, "Stored %s as string: %u (exceeds int32_t max)", name.c_str(), value);
    } else {
        settings.SetInt(name, static_cast<int32_t>(value));
    }

    if (name == "switch") {
        if (set_switch_callback_) {
            set_switch_callback_(value != 0);
            return true;
        }
    } else if (name == "wakeup_word") {
        if (set_wakeup_word_callback_) {
            set_wakeup_word_callback_(value != 0);
            return true;
        }
    } else if (name == "alert_tone_language") {
        if (set_alert_tone_language_callback_) {
            set_alert_tone_language_callback_(static_cast<int>(value));
            return true;
        }
    } else if (name == "chat_mode") {
        if (set_chat_mode_callback_) {
            set_chat_mode_callback_(static_cast<int>(value));
            return true;
        }
    } else if (name == "speed") {
        if (set_speed_callback_) {
            set_speed_callback_(static_cast<int>(value));
            return true;
        }
    } else if (name == "volume_set") {
        if (set_volume_callback_) {
            set_volume_callback_(static_cast<int>(value));
            return true;
        }
    } else if (name == "brightness") {
        if (set_brightness_callback_) {
            set_brightness_callback_(static_cast<int>(value));
            return true;
        }
    } else if (name.find("timer") == 0 && name.length() > 5) {
        // 处理 timer1-timer10
        std::string timer_num_str = name.substr(5); // 提取数字部分
        int timer_index = std::atoi(timer_num_str.c_str());
        if (timer_index >= 1 && timer_index <= 10 && set_timer_callback_) {
            set_timer_callback_(timer_index, value);
            return true;
        }
    }
    return false;
}

// 标准实现：生成上报数据
void GCDataPointManager::GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) {
 
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
    // switch
    if (get_switch_callback_) {
        status |= (get_switch_callback_() ? 1 : 0) << 0;
    } else {
        status |= (1 << 0); // 默认值
    }
    // wakeup_word
    if (get_wakeup_word_callback_) {
        status |= (get_wakeup_word_callback_() ? 1 : 0) << 1;
    } else {
        status |= (1 << 1); // 默认值
    }
    // charge_status
    if (is_charging_callback_) {
        status |= (is_charging_callback_() ? 1 : 0) << 2;
    }
    // alert_tone_language
    if (get_alert_tone_language_callback_) {
        status |= (get_alert_tone_language_callback_() & 0x01) << 4;
    } else {
        status |= (1 << 4); // 默认值
    }
    // chat_mode
    if (get_chat_mode_callback_) {
        status |= (get_chat_mode_callback_() & 0x03) << 5; // 2 bits
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
    
    // 注意：speed 和 timer 数据点在上报数据包中的位置需要根据实际协议格式确定
    // 当前 GenerateReportData 保持原有数据包结构，新数据点通过 GetDataPointValue 获取

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
void GCDataPointManager::ProcessDataPointValue(const std::string& name, uint32_t value) {
    ESP_LOGI(TAG, "ProcessDataPointValue: %s = %u", name.c_str(), value);
    

    if (name == "img_bg") {
        return;
    } else if (name == "video_bg") {
        return;
    } else if (name == "ssid") {
        return;
    }

    SetDataPointValue(name, value);
}

// 将字节数据转换为URL字符串
// 输入：直接是URL的字节值（不是16进制字符串）
// 例如：{0x68, 0x74, 0x74, 0x70, ...} -> "http..."
static std::string HexToUrl(const uint8_t* hex_data, size_t data_len) {
    std::string url;
    
    // 数据直接是URL的字节值，不是16进制字符串
    // 只需要将每个字节转换为字符，遇到0字节（填充）时停止
    
    ESP_LOGI("GCDataPointManager", "HexToUrl: data_len=%zu", data_len);
    
    if (data_len == 0) {
        ESP_LOGW("GCDataPointManager", "HexToUrl: data_len is 0");
        return url;
    }
    
    // 打印前几个字节用于调试
    if (data_len > 0) {
        char debug_buf[64] = {0};
        int debug_len = std::min(static_cast<size_t>(20), data_len);
        for (int i = 0; i < debug_len; i++) {
            snprintf(debug_buf + i * 3, sizeof(debug_buf) - i * 3, "%02X ", hex_data[i]);
        }
        ESP_LOGI("GCDataPointManager", "HexToUrl: first %d bytes (hex): %s", debug_len, debug_buf);
        
        // 打印ASCII表示
        char ascii_buf[64] = {0};
        for (int i = 0; i < debug_len; i++) {
            char c = static_cast<char>(hex_data[i]);
            ascii_buf[i] = (c >= 32 && c < 127) ? c : '.';
        }
        ESP_LOGI("GCDataPointManager", "HexToUrl: first %d bytes (ASCII): %s", debug_len, ascii_buf);
    }
    
    // 直接转换每个字节为字符，遇到0字节（填充）时停止
    for (size_t i = 0; i < data_len; i++) {
        uint8_t byte_val = hex_data[i];
        
        // 如果遇到0字节，说明是填充（URL字符串不应该包含空字符）
        // 停止转换
        if (byte_val == 0) {
            ESP_LOGD("GCDataPointManager", "HexToUrl: found padding at index %zu", i);
            break;
        }
        
        // 将字节值转换为字符并添加到URL
        url += static_cast<char>(byte_val);
    }
    
    // 去除末尾的空字符（如果有）
    while (!url.empty() && url.back() == '\0') {
        url.pop_back();
    }
    
    ESP_LOGI("GCDataPointManager", "HexToUrl: converted URL length=%zu, url='%s'", url.length(), url.c_str());
    
    return url;
}

// 标准实现：处理二进制数据点值
void GCDataPointManager::ProcessBinaryDataPointValue(const std::string& name, const uint8_t* data, size_t data_len) {
    ESP_LOGI(TAG, "ProcessBinaryDataPointValue: %s, len = %zu", name.c_str(), data_len);
    
    // 使用 NVS 进行持久化存储
    Settings settings("datapoint", true);
    
    if (name == "img_bg" || name == "bg_img") {
        // 将16进制字符串转换为URL
        std::string url = HexToUrl(data, data_len);
        // 将 https 改为 http（去掉s）
        size_t https_pos = url.find("https://");
        if (https_pos != std::string::npos) {
            url.replace(https_pos, 5, "http");  // 将 "https" 替换为 "http"
        }
        ESP_LOGI(TAG, "%s URL: %s", name.c_str(), url.c_str());
        
        // 存储到 flash
        settings.SetString("img_bg", url);
        ESP_LOGI(TAG, "Stored img_bg to flash: %s", url.c_str());
        
        if (img_bg_callback_) {
            img_bg_callback_(url);
        }
    } else if (name == "video_bg" || name == "bg_video") {
        // 将16进制字符串转换为URL
        std::string url = HexToUrl(data, data_len);
        // 将 https 改为 http（去掉s）
        size_t https_pos = url.find("https://");
        if (https_pos != std::string::npos) {
            url.replace(https_pos, 5, "http");  // 将 "https" 替换为 "http"
        }
        ESP_LOGI(TAG, "%s URL: %s", name.c_str(), url.c_str());
        
        // 存储到 flash
        settings.SetString("video_bg", url);
        ESP_LOGI(TAG, "Stored video_bg to flash: %s", url.c_str());
        
        if (video_bg_callback_) {
            video_bg_callback_(url);
        }
    } else if (name.find("tts") == 0 && name.length() > 3) {
        // 处理 tts1-tts10
        std::string tts_num_str = name.substr(3); // 提取数字部分
        int tts_index = std::atoi(tts_num_str.c_str());
        if (tts_index >= 1 && tts_index <= 10) {
            // 将二进制数据转换为字符串（遇到0字节停止）
            std::string tts_text;
            for (size_t i = 0; i < data_len; i++) {
                if (data[i] == 0) {
                    break;
                }
                tts_text += static_cast<char>(data[i]);
            }
            ESP_LOGI(TAG, "tts%d text: %s", tts_index, tts_text.c_str());
            
            // 存储到 flash
            std::string tts_key = "tts" + std::to_string(tts_index);
            settings.SetString(tts_key, tts_text);
            ESP_LOGI(TAG, "Stored %s to flash: %s", tts_key.c_str(), tts_text.c_str());
            
            if (set_tts_callback_) {
                set_tts_callback_(tts_index, tts_text);
            }
        }
    }
}

void GCDataPointManager::SetCallbacks(
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
    std::function<uint32_t(int)> get_timer_callback,
    std::function<void(int, uint32_t)> set_timer_callback,
    std::function<void(int, const std::string&)> set_tts_callback
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
    img_bg_callback_ = img_bg_callback;
    video_bg_callback_ = video_bg_callback;
    // 新增数据点的回调函数
    get_switch_callback_ = get_switch_callback;
    set_switch_callback_ = set_switch_callback;
    get_wakeup_word_callback_ = get_wakeup_word_callback;
    set_wakeup_word_callback_ = set_wakeup_word_callback;
    get_alert_tone_language_callback_ = get_alert_tone_language_callback;
    set_alert_tone_language_callback_ = set_alert_tone_language_callback;
    get_speed_callback_ = get_speed_callback;
    set_speed_callback_ = set_speed_callback;
    get_timer_callback_ = get_timer_callback;
    set_timer_callback_ = set_timer_callback;
    set_tts_callback_ = set_tts_callback;
}

void GCDataPointManager::InitFromStorage() {
    // 加载已知可写数据点
    Settings settings("datapoint", false);

    int v;
    
    // switch
    v = settings.GetInt("switch", -1);
    if (v != -1 && set_switch_callback_) {
        cache_["switch"] = v;
        set_switch_callback_(v != 0);
    }

    // wakeup_word
    v = settings.GetInt("wakeup_word", -1);
    if (v != -1 && set_wakeup_word_callback_) {
        cache_["wakeup_word"] = v;
        set_wakeup_word_callback_(v != 0);
    }

    // alert_tone_language
    v = settings.GetInt("alert_tone_language", -1);
    if (v != -1 && set_alert_tone_language_callback_) {
        cache_["alert_tone_language"] = v;
        set_alert_tone_language_callback_(v);
    }

    // chat_mode
    v = settings.GetInt("chat_mode", -1);
    if (v != -1 && set_chat_mode_callback_) {
        cache_["chat_mode"] = v;
        set_chat_mode_callback_(v);
    }

    // speed
    v = settings.GetInt("speed", -1);
    if (v != -1 && set_speed_callback_) {
        cache_["speed"] = v;
        set_speed_callback_(v);
    }

    // volume_set
    v = settings.GetInt("volume_set", -1);
    if (v != -1 && set_volume_callback_) {
        cache_["volume_set"] = v;
        set_volume_callback_(v);
    }

    // brightness
    v = settings.GetInt("brightness", -1);
    if (v != -1 && set_brightness_callback_) {
        cache_["brightness"] = v;
        set_brightness_callback_(v);
    }

    // timer1-timer10
    for (int i = 1; i <= 10; i++) {
        std::string timer_name = "timer" + std::to_string(i);
        v = settings.GetInt(timer_name, -1);
        uint32_t timer_value = 0;
        
        if (v != -1) {
            // 从 int 读取成功
            timer_value = static_cast<uint32_t>(v);
        } else {
            // 尝试从字符串读取（可能超过 int32_t 范围）
            std::string str_value = settings.GetString(timer_name, "");
            if (!str_value.empty()) {
                try {
                    timer_value = static_cast<uint32_t>(std::stoul(str_value));
                    v = static_cast<int>(timer_value); // 用于缓存
                    ESP_LOGI(TAG, "Loaded %s from string: %u", timer_name.c_str(), timer_value);
                } catch (...) {
                    ESP_LOGW(TAG, "Failed to parse %s from string: %s", timer_name.c_str(), str_value.c_str());
                    continue;
                }
            } else {
                continue; // 未设置
            }
        }
        
        if (set_timer_callback_) {
            cache_[timer_name] = v;
            set_timer_callback_(i, timer_value);
        }
    }
}

bool GCDataPointManager::GetCachedDataPoint(const std::string& name, int& value) const {
    auto it = cache_.find(name);
    if (it == cache_.end()) {
        return false;
    }
    value = it->second;
    return true;
}

void GCDataPointManager::SetCachedDataPoint(const std::string& name, int value) {
    cache_[name] = value;
    Settings settings("datapoint", true);
    settings.SetInt(name, value);
}
