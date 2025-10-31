#ifndef _GSEVER_H_
#define _GSEVER_H_

#include <stdint.h>
#include <stdbool.h>
#include "mqtt/mqtt.h"

#define PASSCODE_LEN            10
#define MAC_LEN                 16
#define CLOUD_TOKEN_BIN_LEN     32
#define CLOUD_TOKEN_SZ_LEN      64
#define NETIF_MAX_HWADDR_LEN    6U

// 定义回调函数类型
typedef void (*mqtt_config_callback_t)(mqtt_config_t *config);

// 定义 Onboarding 响应结构
typedef struct {
    int status;           // 响应状态码
    char message[128];    // 响应消息
} onboarding_response_t;

// 定义 Onboarding 回调函数类型
typedef void (*onboarding_callback_t)(onboarding_response_t *response);

// 修改函数声明，添加回调参数
int32_t gatOnboarding(onboarding_callback_t callback);
int32_t gatProvision(mqtt_config_callback_t callback);

#endif /* _GSEVER_H_ */