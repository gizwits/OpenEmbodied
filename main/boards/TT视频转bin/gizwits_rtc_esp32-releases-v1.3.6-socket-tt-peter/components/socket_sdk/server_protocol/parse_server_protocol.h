#pragma once

#include <stdint.h>
#include <stdbool.h>

// 命令字
#define CMD_VERSION_REPORT_RESP    0x021d

// 命令标识位
#define FLAG_HAS_SUBDEV      (1 << 0)  // bit0: 子设备标识位
#define FLAG_HAS_MODULE_VER  (1 << 1)  // bit1: 模组版本标识位
#define FLAG_HAS_MCU_VER     (1 << 2)  // bit2: MCU版本标识位
#define FLAG_HAS_FILE_MD5    (1 << 8)  // bit8: 文件MD5
#define FLAG_HAS_HTTP_LINK   (1 << 9)  // bit9: 固件HTTP下载链接
#define FLAG_HAS_HTTPS_LINK  (1 << 10) // bit10: 固件HTTPS下载链接

// 版本信息结构体
typedef struct {
    char subdev_id[32];        // 子设备ID
    char module_hw_ver[16];    // 模组硬件版本
    char module_sw_ver[16];    // 模组软件版本
    char mcu_hw_ver[16];       // MCU硬件版本
    char mcu_sw_ver[16];       // MCU软件版本
    char file_md5[33];         // 文件MD5 (32字节+'\0')
    char download_url[256];    // 固件下载链接
} version_server_info_t;

// 统一的数据结构
typedef struct {
    bool success;          // 解析是否成功
    uint16_t cmd;         // 命令字
    uint16_t flags;       // 命令标识位
    union {
        version_server_info_t version_info;  // 版本信息
        // 可以添加其他命令对应的数据结构
    } data;
} server_protocol_data_t;

// 只暴露一个解析入口
server_protocol_data_t server_protocol_parse_data(const uint8_t *buf, size_t len);
