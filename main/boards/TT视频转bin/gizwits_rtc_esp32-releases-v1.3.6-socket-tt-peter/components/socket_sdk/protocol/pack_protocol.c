#include <string.h>
#include "pack_protocol.h"
#include "config.h"
#include "esp_log.h"
#include "sdk_api.h"

static const char *TAG = "PACK_PROTO";

// WiFi配置响应包结构
typedef struct {
    uint8_t msg_id;         // 消息ID和版本 (0x04)
    uint8_t cmd;            // 命令字 (0x41)
    uint8_t frame_seq;      // 帧序号
    uint8_t frame_len;      // 帧长度
    uint8_t status;         // 状态
    char hw_ver[8];         // 硬件版本
    char sw_ver[8];         // 软件版本
} __attribute__((packed)) wifi_config_response_t;

size_t pack_wifi_config_response(uint8_t frame_seq,uint8_t msg_id, uint8_t status,
                               uint8_t *out_buf, size_t buf_size)
{
    if (!out_buf || buf_size < sizeof(wifi_config_response_t)) {
        ESP_LOGE(TAG, "Invalid buffer or size");
        return 0;
    }

    wifi_config_response_t *resp = (wifi_config_response_t *)out_buf;
    
    // 填充响应包
    resp->msg_id = msg_id;
    resp->cmd = CMD_WIFI_CONFIG_RESP;
    resp->frame_seq = frame_seq;
    resp->frame_len = sizeof(wifi_config_response_t) - 4;  // 减去前4个字节
    resp->status = status;
    
    // 复制版本信息
    strncpy(resp->hw_ver, sdk_get_hardware_version(), sizeof(resp->hw_ver));
    strncpy(resp->sw_ver, sdk_get_software_version(), sizeof(resp->sw_ver));

    ESP_LOGD(TAG, "Built WiFi config response: status=0x%02x, seq=%d", 
             status, frame_seq);

    return sizeof(wifi_config_response_t);
}
