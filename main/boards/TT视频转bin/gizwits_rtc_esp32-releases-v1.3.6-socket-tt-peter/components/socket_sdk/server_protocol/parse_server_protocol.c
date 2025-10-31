#include <string.h>
#include <stdio.h>
#include "parse_server_protocol.h"
#include "esp_log.h"

static const char* TAG = "ParseServerProtocol";

// 服务器协议头结构体
typedef struct {
    uint32_t fixed_header;  // 固定包头
    uint16_t cmd;          // 命令字
    uint16_t flags;        // 命令标识位
    uint16_t data_len;     // 命令数据长度
} server_protocol_header_t;

// 从buffer中读取2字节的值
static uint16_t read_uint16(const uint8_t *buf) {
    return (buf[0] << 8) | buf[1];
}

// 从buffer中读取4字节的值
static uint32_t read_uint32(const uint8_t *buf) {
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

// 解析协议头
static bool parse_protocol_header(const uint8_t *data, size_t len, 
                                server_protocol_header_t *header) {
    if (len < 10) {
        ESP_LOGE(TAG, "Buffer too short for header");
        return false;
    }

    const uint8_t *p = data;
    header->fixed_header = read_uint32(p);
    p += 4;
    header->cmd = read_uint16(p);
    p += 2;
    header->flags = read_uint16(p);
    p += 2;
    header->data_len = read_uint16(p);

    ESP_LOGI(TAG, "Protocol Header: fixed=0x%08x, cmd=0x%04x, flags=0x%04x, len=%d",
             header->fixed_header, header->cmd, header->flags, header->data_len);

    return true;
}

// 通用的字段解析函数
static bool parse_version_field(const uint8_t **p, size_t *remaining_len,
                              char *hw_ver, char *sw_ver, 
                              size_t hw_size, size_t sw_size,
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
        char ver_buf[64] = {0};
        if (field_len >= sizeof(ver_buf)) {
            ESP_LOGE(TAG, "%s data too long", field_name);
            return false;
        }

        memcpy(ver_buf, *p, field_len);
        ver_buf[field_len] = '\0';
        
        char *comma = strchr(ver_buf, ',');
        if (comma) {
            *comma = '\0';
            strncpy(hw_ver, ver_buf, hw_size - 1);
            strncpy(sw_ver, comma + 1, sw_size - 1);
            ESP_LOGD(TAG, "%s HW: %s, SW: %s", field_name, hw_ver, sw_ver);
        } else {
            ESP_LOGE(TAG, "Invalid %s format", field_name);
            return false;
        }

        *p += field_len;
        *remaining_len -= field_len;
    }

    return true;
}

// 通用的字符串字段解析函数
static bool parse_string_field(const uint8_t **p, size_t *remaining_len,
                             char *out_str, size_t out_size,
                             const char *field_name) {
    if (*remaining_len < 2) {
        ESP_LOGE(TAG, "No enough data for %s length", field_name);
        return false;
    }

    uint16_t field_len = read_uint16(*p);
    *p += 2;
    *remaining_len -= 2;

    if (field_len > *remaining_len || field_len >= out_size) {
        ESP_LOGE(TAG, "%s length exceeds buffer size", field_name);
        return false;
    }

    if (field_len > 0) {
        memcpy(out_str, *p, field_len);
        out_str[field_len] = '\0';
        *p += field_len;
        *remaining_len -= field_len;
        ESP_LOGD(TAG, "%s: %s", field_name, out_str);
    }

    return true;
}

// 解析版本上报数据
static int parse_version_report_data(const uint8_t *buf, int buf_len, 
                                   const server_protocol_header_t *header,
                                   server_protocol_data_t *result) {
    const uint8_t *p = buf + 10;  // 跳过协议头
    size_t remaining_len = buf_len - 10;
    version_server_info_t *ver_info = &result->data.version_info;

    // 解析子设备ID
    if (header->flags & FLAG_HAS_SUBDEV) {
        if (!parse_string_field(&p, &remaining_len,
                              ver_info->subdev_id, sizeof(ver_info->subdev_id),
                              "Subdev ID")) {
            return -1;
        }
    }

    // 解析模组版本信息
    if (header->flags & FLAG_HAS_MODULE_VER) {
        if (!parse_version_field(&p, &remaining_len,
                               ver_info->module_hw_ver, ver_info->module_sw_ver,
                               sizeof(ver_info->module_hw_ver), 
                               sizeof(ver_info->module_sw_ver),
                               "Module Version")) {
            return -2;
        }
    }

    // 解析MCU版本信息
    if (header->flags & FLAG_HAS_MCU_VER) {
        if (!parse_version_field(&p, &remaining_len,
                               ver_info->mcu_hw_ver, ver_info->mcu_sw_ver,
                               sizeof(ver_info->mcu_hw_ver), 
                               sizeof(ver_info->mcu_sw_ver),
                               "MCU Version")) {
            return -3;
        }
    }

    // 解析文件MD5
    if (header->flags & FLAG_HAS_FILE_MD5) {
        if (!parse_string_field(&p, &remaining_len,
                              ver_info->file_md5, sizeof(ver_info->file_md5),
                              "File MD5")) {
            return -4;
        }
    }

    // 解析固件下载链接
    if (header->flags & FLAG_HAS_HTTP_LINK) {
        if (!parse_string_field(&p, &remaining_len,
                              ver_info->download_url, sizeof(ver_info->download_url),
                              "HTTP Download URL")) {
            return -5;
        }
    } else if (header->flags & FLAG_HAS_HTTPS_LINK) {
        if (!parse_string_field(&p, &remaining_len,
                              ver_info->download_url, sizeof(ver_info->download_url),
                              "HTTPS Download URL")) {
            return -6;
        }
    }

    return 0;
}

// 对外暴露的唯一解析入口
server_protocol_data_t server_protocol_parse_data(const uint8_t *buf, size_t len) 
{
    server_protocol_data_t result = {0};
    result.success = false;

    if (!buf || len < 10) {
        ESP_LOGE(TAG, "Invalid parameters");
        return result;
    }

    // 解析协议头
    server_protocol_header_t header;
    if (!parse_protocol_header(buf, len, &header)) {
        return result;
    }

    // 设置基本信息
    result.cmd = header.cmd;
    result.flags = header.flags;

    // 根据命令类型选择对应的解析方法
    switch (header.cmd) {
        case CMD_VERSION_REPORT_RESP:
            result.success = (parse_version_report_data(buf, len, &header, &result) == 0);
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown command: 0x%04x", header.cmd);
            // 对于未知命令,可以选择是否解析原始数据
            break;
    }

    return result;
}