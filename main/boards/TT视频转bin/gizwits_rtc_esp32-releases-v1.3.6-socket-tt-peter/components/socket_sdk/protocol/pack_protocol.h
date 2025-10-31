#ifndef _PACK_PROTOCOL_H_
#define _PACK_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 消息ID定义
#define MSG_ID_DATA_POINT    0x04

// 命令定义
#define CMD_WIFI_CONFIG_RESP      0x41

// 响应状态码
#define RESP_STATUS_OK       0x00
#define RESP_STATUS_ERROR    0x80

/**
 * @brief 构建WiFi配置响应包
 * @param frame_seq 原请求的帧序号
 * @param status 响应状态 (0=成功, 0x80=失败)
 * @param out_buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 响应包长度，失败返回0
 */
size_t pack_wifi_config_response(uint8_t frame_seq, uint8_t msg_id,uint8_t status, 
                               uint8_t *out_buf, size_t buf_size);

#endif /* _PACK_PROTOCOL_H_ */
