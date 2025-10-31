#include <string.h>
#include <stdio.h>
#include "pack_server_protocol.h"
#include "esp_log.h"

static const char* TAG = "ServerProtocol";

// 写入2字节长度
static inline void write_uint16(uint8_t *buf, uint16_t value) {
    buf[0] = (value >> 8) & 0xFF;
    buf[1] = value & 0xFF;
}

// 写入4字节长度
static inline void write_uint32(uint8_t *buf, uint32_t value) {
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8) & 0xFF;
    buf[3] = value & 0xFF;
}

int pack_version_report(uint8_t *buf, int buf_len, const version_info_t *ver_info)
{
    if (!buf || !ver_info || buf_len < 10) {
        return -1;
    }

    uint8_t *p = buf;
    uint16_t flags = 0;
    uint16_t total_len = 0;
    char module_ver[64] = {0};
    char mcu_ver[64] = {0};

    // 1. 写入固定包头
    write_uint32(p, FIXED_HEADER);
    p += 4;

    // 2. 写入命令字
    write_uint16(p, CMD_VERSION_REPORT);
    p += 2;

    // 计算标志位和准备版本字符串
    if (ver_info->subdev_id && strlen(ver_info->subdev_id) > 0) {
        flags |= FLAG_HAS_SUBDEV;
    }
    if (ver_info->module_hw_ver && ver_info->module_sw_ver) {
        flags |= FLAG_HAS_MODULE_VER;
        snprintf(module_ver, sizeof(module_ver), "%s,%s", 
                ver_info->module_hw_ver, ver_info->module_sw_ver);
    }
    if (ver_info->mcu_hw_ver && ver_info->mcu_sw_ver) {
        flags |= FLAG_HAS_MCU_VER;
        snprintf(mcu_ver, sizeof(mcu_ver), "%s,%s", 
                ver_info->mcu_hw_ver, ver_info->mcu_sw_ver);
    }
    // 3. 写入命令标识位
    write_uint16(p, flags);
    p += 2;

    // 保存数据长度位置
    uint8_t *len_pos = p;
    p += 2;

    // 4. 写入子设备信息 (即使标志位未设置也写入长度0)
    if (flags & FLAG_HAS_SUBDEV) {
        uint16_t subdev_len = strlen(ver_info->subdev_id);
        write_uint16(p, subdev_len);
        p += 2;
        memcpy(p, ver_info->subdev_id, subdev_len);
        p += subdev_len;
    } else {
        // 子设备ID为空时
    }

    // 5. 写入模组版本信息
    if (flags & FLAG_HAS_MODULE_VER) {
        uint16_t module_ver_len = strlen(module_ver);
        write_uint16(p, module_ver_len);
        p += 2;
        memcpy(p, module_ver, module_ver_len);
        p += module_ver_len;
    }

    // 6. 写入MCU版本信息
    if (flags & FLAG_HAS_MCU_VER) {
        uint16_t mcu_ver_len = strlen(mcu_ver);
        write_uint16(p, mcu_ver_len);
        p += 2;
        memcpy(p, mcu_ver, mcu_ver_len);
        p += mcu_ver_len;
    }

    // 计算并写入总长度
    total_len = p - len_pos - 2;
    write_uint16(len_pos, total_len);

    return p - buf;
}
