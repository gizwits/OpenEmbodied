#include "ota_protocol.h"
#include <esp_log.h>
#include <cstring>
#include <cstdio>

#define TAG "OTA_PROTOCOL"

namespace ota {
namespace protocol {

size_t pack_mqtt_upgrade_progress(
    uint16_t cmd_flag,
    uint8_t progress,
    const char* hw_version,
    const char* sw_version,
    const char* upgrade_info,
    uint8_t* out_buf,
    size_t buf_size
) {
    if (!out_buf || !hw_version || !sw_version || !upgrade_info) {
        ESP_LOGE(TAG, "Invalid input");
        return 0;
    }

    // 拼接版本号字符串
    char pending_version[32] = {0};
    snprintf(pending_version, sizeof(pending_version), "%s,%s", hw_version, sw_version);
    size_t pending_version_len = strlen(pending_version);
    size_t upgrade_info_len = strlen(upgrade_info);
    
    // 计算数据部分长度 (不包括头部和长度字段)
    uint16_t data_len = 1 + 2 + pending_version_len + 2 + upgrade_info_len;  // 进度(1) + 版本长度(2) + 版本内容 + 状态长度(2) + 状态内容
    
    // 计算总长度
    size_t total_len = 4 + 2 + 2 + 2 + data_len;  // 头(4) + cmd(2) + flag(2) + 长度(2) + 数据部分
    
    if (buf_size < total_len) {
        ESP_LOGE(TAG, "Buffer too small");
        return 0;
    }

    uint8_t* p = out_buf;
    // 固定包头
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x03;
    // 命令字
    *p++ = 0x02; *p++ = 0x2c;
    // 命令标识位
    *p++ = (cmd_flag >> 8) & 0xFF;
    *p++ = cmd_flag & 0xFF;
    // 命令数据长度
    *p++ = (data_len >> 8) & 0xFF;
    *p++ = data_len & 0xFF;

    // 升级进度
    *p++ = progress;
    // 待升级版本长度
    *p++ = (pending_version_len >> 8) & 0xFF;
    *p++ = pending_version_len & 0xFF;
    // 待升级版本
    memcpy(p, pending_version, pending_version_len); p += pending_version_len;
    // 升级信息长度
    *p++ = (upgrade_info_len >> 8) & 0xFF;
    *p++ = upgrade_info_len & 0xFF;
    // 升级信息
    memcpy(p, upgrade_info, upgrade_info_len); p += upgrade_info_len;

    ESP_LOGD(TAG, "Built MQTT upgrade progress packet, total_len=%d", (int)(p - out_buf));
    return (size_t)(p - out_buf);
}

} // namespace protocol
} // namespace ota 