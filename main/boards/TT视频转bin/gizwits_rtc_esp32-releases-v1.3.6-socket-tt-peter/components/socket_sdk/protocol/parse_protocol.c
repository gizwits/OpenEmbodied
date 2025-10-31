#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "parse_protocol.h"

// 协议格式定义
typedef struct {
    struct {
        uint8_t msg_id: 5;    // Bit 0-4: Msg ID (1-31)
        uint8_t ver: 2;       // Bit 5-6: Payload格式版本
        uint8_t reserved: 1;  // Bit 7: 保留位
    } byte0;
    uint8_t cmd;             // 指令值
    struct {
        uint8_t seq: 4;      // Bit 0-3: 帧序号 (0-15)
        uint8_t frames: 4;   // Bit 4-7: 总拆包帧数 (实际帧数=值+1)
    } byte2;
    uint8_t frame_len;       // 帧数据长度
} protocol_header_t;

// 解析协议头
static void parse_protocol_header(const uint8_t *data, protocol_header_t *header) {
    // 解析第一个字节
    header->byte0.msg_id = data[0] & 0x1F;        // 取低5位
    header->byte0.ver = (data[0] >> 5) & 0x03;    // 取第5-6位
    header->byte0.reserved = (data[0] >> 7) & 0x01;// 取第7位

    // 解析第二个字节 (CMD)
    header->cmd = data[1];

    // 解析第三个字节
    header->byte2.seq = data[2] & 0x0F;           // 取低4位
    header->byte2.frames = (data[2] >> 4) & 0x0F; // 取高4位

    // 解析第四个字节 (帧长度)
    header->frame_len = data[3];
}

// 打印协议头信息
static void print_protocol_header(const protocol_header_t *header) {
    printf("Protocol Header:\n");
    printf("  Msg ID: %d\n", header->byte0.msg_id);
    printf("  Version: %d\n", header->byte0.ver);
    printf("  Reserved: %d\n", header->byte0.reserved);
    printf("  CMD: 0x%02X\n", header->cmd);
    printf("  Sequence: %d\n", header->byte2.seq);
    printf("  Total Frames: %d\n", header->byte2.frames + 1);
    printf("  Frame Length: %d\n", header->frame_len);
}

// 通用的字段解析函数
static bool parse_field(const uint8_t *data, size_t data_len, size_t *offset, 
                       uint8_t *out_len, void *out_data, size_t max_len, 
                       const char *field_name) {
    // 检查长度字段
    if (*offset + 1 > data_len) {
        printf("Error: Not enough data for %s length. Required: %zu, Available: %zu\n", 
               field_name, 1, data_len - *offset);
        return false;
    }
    
    // 获取字段长度
    *out_len = data[(*offset)++];
    printf("%s Length: %d\n", field_name, *out_len);
    
    // 检查字段数据
    if (*out_len > 0) {
        if (*offset + *out_len > data_len || *out_len > max_len) {
            printf("Error: %s length exceeds limits. Length: %d, Available: %zu, Max: %zu\n", 
                   field_name, *out_len, data_len - *offset, max_len);
            return false;
        }
        memcpy(out_data, &data[*offset], *out_len);
        if (max_len > *out_len) {  // 如果是字符串，添加结束符
            ((uint8_t*)out_data)[*out_len] = '\0';
        }
        *offset += *out_len;
        
        // 打印解析结果
        if (field_name[0] != '_') {  // 如果字段名不以_开头就打印
            printf("Parsed %s: %s\n", field_name, (char*)out_data);
        }
    }
    
    return true;
}

static bool parse_wifi_config(const uint8_t *data, size_t len, wifi_config_t *wifi_config) {
    size_t offset = 4;  // 跳过协议头

    // 1. 解析NTP时间戳
    if (offset + 4 > len) {
        printf("Error: Not enough data for NTP timestamp\n");
        return false;
    }
    wifi_config->ntp = (data[offset] << 24) | (data[offset+1] << 16) | 
                      (data[offset+2] << 8) | data[offset+3];
    offset += 4;
    printf("Parsed NTP: %u\n", wifi_config->ntp);

    // 2. 解析SSID
    if (!parse_field(data, len, &offset, &wifi_config->ssid_len, 
                    wifi_config->ssid, sizeof(wifi_config->ssid) - 1, "SSID")) {
        return false;
    }

    // 3. 解析BSSID
    if (!parse_field(data, len, &offset, &wifi_config->bssid_len,
                    wifi_config->bssid, sizeof(wifi_config->bssid) - 1, "BSSID")) {
        return false;
    }

    // 4. 解析密码
    if (!parse_field(data, len, &offset, &wifi_config->password_len,
                    wifi_config->password, sizeof(wifi_config->password) - 1, "Password")) {
        return false;
    }

    // 5. 解析UID
    if (!parse_field(data, len, &offset, &wifi_config->uid_len,
                    wifi_config->uid, sizeof(wifi_config->uid) - 1, "UID")) {
        return false;
    }

    // 打印最终配置摘要
    printf("\nWiFi Configuration Summary:\n");
    printf("  NTP: %u\n", wifi_config->ntp);
    printf("  SSID (%d): %s\n", wifi_config->ssid_len, wifi_config->ssid);
    if (wifi_config->bssid_len > 0) {
        printf("  BSSID (%d): ", wifi_config->bssid_len);
        for (int i = 0; i < wifi_config->bssid_len; i++) {
            printf("%02X", (uint8_t)wifi_config->bssid[i]);
            if (i < wifi_config->bssid_len - 1) printf(":");
        }
        printf("\n");
    }
    printf("  Password (%d): %s\n", wifi_config->password_len, wifi_config->password);
    printf("  UID (%d): %s\n", wifi_config->uid_len, wifi_config->uid);

    return true;
}

// 解析协议数据
protocol_data_t protocol_parse_data(const uint8_t *data, size_t len) {
    protocol_data_t result = {0};
    result.success = false;

    if (len < 4) {  // 确保至少有协议头的长度
        printf("Invalid protocol data length\n");
        return result;
    }

    // 解析协议头
    protocol_header_t header;
    parse_protocol_header(data, &header);
    
    // 打印协议头信息
    print_protocol_header(&header);

    // 设置命令类型
    result.cmd = header.cmd;
    result.msg_id = header.byte0.msg_id;

    // 根据CMD选择不同的解析方法
    switch (header.cmd) {
        case CMD_WIFI_CONFIG:
            result.success = parse_wifi_config(data, len, &result.data.wifi_config);
            break;
        default:
            // 如果有数据部分，打印原始数据
            if (len > 4 && header.frame_len > 0) {
                printf("Payload Data: ");
                for (int i = 4; i < len && i < (4 + header.frame_len); i++) {
                    printf("%02X ", data[i]);
                }
                printf("\n");
            }
            break;
    }

    return result;
}
