#include <string>
#include <cstring>
#include <cstdint>
#include <esp_log.h>

namespace iot {
namespace protocol {

static const char* TAG = "ServerProtocol";

// Constants
constexpr uint32_t FIXED_HEADER = 0x00000003;
constexpr uint16_t CMD_VERSION_REPORT = 0x021c;
constexpr uint16_t CMD_VERSION_REPORT_RESP = 0x021d;

// Command flags
constexpr uint16_t FLAG_HAS_SUBDEV = (1 << 0);
constexpr uint16_t FLAG_HAS_MODULE_VER = (1 << 1);
constexpr uint16_t FLAG_HAS_MCU_VER = (1 << 2);
constexpr uint16_t FLAG_HAS_FILE_MD5 = (1 << 8);
constexpr uint16_t FLAG_HAS_HTTP_LINK = (1 << 9);
constexpr uint16_t FLAG_HAS_HTTPS_LINK = (1 << 10);

// Helper functions for reading/writing integers
namespace {
    inline void write_uint16(uint8_t *buf, uint16_t value) {
        buf[0] = (value >> 8) & 0xFF;
        buf[1] = value & 0xFF;
    }

    inline void write_uint32(uint8_t *buf, uint32_t value) {
        buf[0] = (value >> 24) & 0xFF;
        buf[1] = (value >> 16) & 0xFF;
        buf[2] = (value >> 8) & 0xFF;
        buf[3] = value & 0xFF;
    }

    inline uint16_t read_uint16(const uint8_t *buf) {
        return (buf[0] << 8) | buf[1];
    }

    inline uint32_t read_uint32(const uint8_t *buf) {
        return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    }
}

// Version information structure
struct VersionInfo {
    std::string subdev_id;         // 子设备ID
    std::string module_hw_ver;     // 模组硬件版本
    std::string module_sw_ver;     // 模组软件版本
    std::string mcu_hw_ver;        // MCU硬件版本
    std::string mcu_sw_ver;        // MCU软件版本
    std::string file_md5;          // 文件MD5
    std::string download_url;      // 固件下载链接
};

// Protocol header structure
struct ProtocolHeader {
    uint32_t fixed_header;  // 固定包头
    uint16_t cmd;          // 命令字
    uint16_t flags;        // 命令标识位
    uint16_t data_len;     // 命令数据长度
};

// Protocol data structure
struct ProtocolData {
    bool success;          // 解析是否成功
    uint16_t cmd;         // 命令字
    uint16_t flags;       // 命令标识位
    VersionInfo version_info;  // 版本信息
};

// Pack version report
int pack_version_report(uint8_t *buf, int buf_len, const VersionInfo &ver_info) {
    if (!buf || buf_len < 10) {
        return -1;
    }

    uint8_t *p = buf;
    uint16_t flags = 0;
    uint16_t total_len = 0;
    std::string module_ver;
    std::string mcu_ver;

    // 1. Write fixed header
    write_uint32(p, FIXED_HEADER);
    p += 4;

    // 2. Write command
    write_uint16(p, CMD_VERSION_REPORT);
    p += 2;

    // Calculate flags and prepare version strings
    if (!ver_info.subdev_id.empty()) {
        flags |= FLAG_HAS_SUBDEV;
    }
    if (!ver_info.module_hw_ver.empty() && !ver_info.module_sw_ver.empty()) {
        flags |= FLAG_HAS_MODULE_VER;
        module_ver = ver_info.module_hw_ver + "," + ver_info.module_sw_ver;
    }
    if (!ver_info.mcu_hw_ver.empty() && !ver_info.mcu_sw_ver.empty()) {
        flags |= FLAG_HAS_MCU_VER;
        mcu_ver = ver_info.mcu_hw_ver + "," + ver_info.mcu_sw_ver;
    }

    // 3. Write flags
    write_uint16(p, flags);
    p += 2;

    // Save length position
    uint8_t *len_pos = p;
    p += 2;

    // 4. Write subdevice info
    if (flags & FLAG_HAS_SUBDEV) {
        uint16_t subdev_len = ver_info.subdev_id.length();
        write_uint16(p, subdev_len);
        p += 2;
        memcpy(p, ver_info.subdev_id.c_str(), subdev_len);
        p += subdev_len;
    }

    // 5. Write module version info
    if (flags & FLAG_HAS_MODULE_VER) {
        uint16_t module_ver_len = module_ver.length();
        write_uint16(p, module_ver_len);
        p += 2;
        memcpy(p, module_ver.c_str(), module_ver_len);
        p += module_ver_len;
    }

    // 6. Write MCU version info
    if (flags & FLAG_HAS_MCU_VER) {
        uint16_t mcu_ver_len = mcu_ver.length();
        write_uint16(p, mcu_ver_len);
        p += 2;
        memcpy(p, mcu_ver.c_str(), mcu_ver_len);
        p += mcu_ver_len;
    }

    // Calculate and write total length
    total_len = p - len_pos - 2;
    write_uint16(len_pos, total_len);

    return p - buf;
}

// Parse protocol header
bool parse_protocol_header(const uint8_t *data, size_t len, ProtocolHeader &header) {
    if (len < 10) {
        ESP_LOGE(TAG, "Buffer too short for header");
        return false;
    }

    const uint8_t *p = data;
    header.fixed_header = read_uint32(p);
    p += 4;
    header.cmd = read_uint16(p);
    p += 2;
    header.flags = read_uint16(p);
    p += 2;
    header.data_len = read_uint16(p);

    ESP_LOGI(TAG, "Protocol Header: fixed=0x%08lx, cmd=0x%04x, flags=0x%04x, len=%d",
             header.fixed_header, header.cmd, header.flags, header.data_len);

    return true;
}

// Parse version field
bool parse_version_field(const uint8_t **p, size_t *remaining_len,
                        std::string &hw_ver, std::string &sw_ver,
                        const char *field_name) {
    if (*remaining_len < 2) {
        ESP_LOGE(TAG, "No enough data for %s length", field_name);
        return false;
    }

    uint16_t field_len = read_uint16(*p);
    *p += 2;
    *remaining_len -= 2;

    if (field_len > *remaining_len) {
        ESP_LOGE(TAG, "%s data length exceeds buffer size", field_name);
        return false;
    }

    if (field_len > 0) {
        std::string ver_buf(reinterpret_cast<const char*>(*p), field_len);
        
        size_t comma_pos = ver_buf.find(',');
        if (comma_pos != std::string::npos) {
            hw_ver = ver_buf.substr(0, comma_pos);
            sw_ver = ver_buf.substr(comma_pos + 1);
            ESP_LOGD(TAG, "%s HW: %s, SW: %s", field_name, hw_ver.c_str(), sw_ver.c_str());
        } else {
            ESP_LOGE(TAG, "Invalid %s format", field_name);
            return false;
        }

        *p += field_len;
        *remaining_len -= field_len;
    }

    return true;
}

// Parse string field
bool parse_string_field(const uint8_t **p, size_t *remaining_len,
                       std::string &out_str,
                       const char *field_name) {
    if (*remaining_len < 2) {
        ESP_LOGE(TAG, "No enough data for %s length", field_name);
        return false;
    }

    uint16_t field_len = read_uint16(*p);
    *p += 2;
    *remaining_len -= 2;

    if (field_len > *remaining_len) {
        ESP_LOGE(TAG, "%s length exceeds buffer size", field_name);
        return false;
    }

    if (field_len > 0) {
        out_str = std::string(reinterpret_cast<const char*>(*p), field_len);
        *p += field_len;
        *remaining_len -= field_len;
        ESP_LOGD(TAG, "%s: %s", field_name, out_str.c_str());
    }

    return true;
}

// Parse version report data
int parse_version_report_data(const uint8_t *buf, int buf_len,
                            const ProtocolHeader &header,
                            ProtocolData &result) {
    const uint8_t *p = buf + 10;  // Skip protocol header
    size_t remaining_len = buf_len - 10;

    // Parse subdevice ID
    if (header.flags & FLAG_HAS_SUBDEV) {
        if (!parse_string_field(&p, &remaining_len,
                              result.version_info.subdev_id,
                              "Subdev ID")) {
            return -1;
        }
    }

    // Parse module version info
    if (header.flags & FLAG_HAS_MODULE_VER) {
        if (!parse_version_field(&p, &remaining_len,
                               result.version_info.module_hw_ver,
                               result.version_info.module_sw_ver,
                               "Module Version")) {
            return -2;
        }
    }

    // Parse MCU version info
    if (header.flags & FLAG_HAS_MCU_VER) {
        if (!parse_version_field(&p, &remaining_len,
                               result.version_info.mcu_hw_ver,
                               result.version_info.mcu_sw_ver,
                               "MCU Version")) {
            return -3;
        }
    }

    // Parse file MD5
    if (header.flags & FLAG_HAS_FILE_MD5) {
        if (!parse_string_field(&p, &remaining_len,
                              result.version_info.file_md5,
                              "File MD5")) {
            return -4;
        }
    }

    // Parse download URL
    if (header.flags & FLAG_HAS_HTTP_LINK) {
        if (!parse_string_field(&p, &remaining_len,
                              result.version_info.download_url,
                              "HTTP Download URL")) {
            return -5;
        }
    } else if (header.flags & FLAG_HAS_HTTPS_LINK) {
        if (!parse_string_field(&p, &remaining_len,
                              result.version_info.download_url,
                              "HTTPS Download URL")) {
            return -6;
        }
    }

    return 0;
}

// Main protocol parse function
ProtocolData parse_protocol_data(const uint8_t *buf, size_t len) {
    ProtocolData result;
    result.success = false;

    if (!buf || len < 10) {
        ESP_LOGE(TAG, "Invalid parameters");
        return result;
    }

    // Parse protocol header
    ProtocolHeader header;
    if (!parse_protocol_header(buf, len, header)) {
        return result;
    }

    // Set basic information
    result.cmd = header.cmd;
    result.flags = header.flags;

    // Parse based on command type
    switch (header.cmd) {
        case CMD_VERSION_REPORT_RESP:
            result.success = (parse_version_report_data(buf, len, header, result) == 0);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%04x", header.cmd);
            break;
    }

    return result;
}

} // namespace protocol
} // namespace iot
