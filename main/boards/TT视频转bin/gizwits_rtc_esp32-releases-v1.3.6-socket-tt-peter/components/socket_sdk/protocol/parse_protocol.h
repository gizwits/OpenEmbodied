#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// 协议版本定义
#define PROTOCOL_VER_GIZWITS    0   // 机智云数据点协议
#define PROTOCOL_VER_PROTOBUF   1   // Protobuf协议
#define PROTOCOL_VER_PASSTHROUGH 2   // 透传协议

// CMD 指令定义
#define CMD_WIFI_CONFIG 0x40        // WiFi配置指令

// WiFi配置结构体
typedef struct {
    uint32_t ntp;           // 时间戳
    char ssid[33];          // SSID (最大32字节 + 1字节结束符)
    uint8_t ssid_len;       // SSID长度
    char bssid[7];          // BSSID (6字节 + 1字节结束符)
    uint8_t bssid_len;      // BSSID长度
    char password[65];      // 密码 (最大64字节 + 1字节结束符)
    uint8_t password_len;   // 密码长度
    char uid[33];           // UID (最大32字节 + 1字节结束符)
    uint8_t uid_len;        // UID长度
} wifi_config_t;

// 协议解析结果结构体
typedef struct {
    uint8_t cmd;           // 命令类型
    uint8_t msg_id;        // 消息ID
    bool success;          // 解析是否成功
    union {
        wifi_config_t wifi_config;  // WiFi配置数据
        // 可以在这里添加其他类型的数据结构
    } data;
} protocol_data_t;

// 字段类型枚举
typedef enum {
    FIELD_TYPE_STRING,
    FIELD_TYPE_BYTES,
    FIELD_TYPE_UINT32,
    // 可以添加更多类型...
} field_type_t;

// 字段描述结构
typedef struct {
    const char *name;     // 字段名称
    field_type_t type;    // 字段类型
    size_t offset;        // 字段在结构体中的偏移
    size_t max_len;       // 最大长度（对于字符串和字节数组）
} field_desc_t;

// 函数声明
protocol_data_t protocol_parse_data(const uint8_t *data, size_t len);

#endif // _PROTOCOL_H_ 