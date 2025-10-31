#ifndef _SERVER_PROTOCOL_H_
#define _SERVER_PROTOCOL_H_

#include <stdint.h>

// 命令字定义
#define CMD_VERSION_REPORT    0x021c
#define FIXED_HEADER         0x00000003

// 命令标识位
#define FLAG_HAS_SUBDEV      (1 << 0)
#define FLAG_HAS_MODULE_VER  (1 << 1)
#define FLAG_HAS_MCU_VER     (1 << 2)

// 版本信息结构体
typedef struct {
    char *subdev_id;         // 子设备ID
    char *module_hw_ver;     // 模组硬件版本
    char *module_sw_ver;     // 模组软件版本
    char *mcu_hw_ver;        // MCU硬件版本
    char *mcu_sw_ver;        // MCU软件版本
} version_info_t;

// 函数声明
int pack_version_report(uint8_t *buf, int buf_len, const version_info_t *ver_info);

#endif
