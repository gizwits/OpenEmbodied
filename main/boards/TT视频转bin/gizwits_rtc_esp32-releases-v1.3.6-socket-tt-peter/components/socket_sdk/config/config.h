#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "cJSON.h"
#include <stdint.h>

#define PK_LEN           32
#define PKS_LEN          32
#define DID_LEN          22
#define AUTHkEY_LEN      32
#define MAC_LEN          16


// 产品信息结构体
typedef struct _productinfo_st{
    uint8_t szPK[PK_LEN + 1];        // 产品密钥
    uint8_t szPKs[PKS_LEN + 1];       // 产品密钥
    uint8_t szDID[DID_LEN + 1];       // 设备ID
    uint8_t szAuthKey[AUTHkEY_LEN + 1];   // 认证密钥
    uint8_t szMac[MAC_LEN + 1];       // MAC地址
} product_info_t;


// 产品配置
#define PRODUCT_KEY     "8179cb7ac34649fe9eaa735892aed562"  // 产品密钥
#define PRODUCT_SECRET  "57125a1eee4c484ebbd806406201dc5a"  // 产品密钥

#define TEST_MAC        "e80690a84e6c"  // MAC地址
#define TEST_USER     "6499e3651fd94dc6926a4103391e9b7e"  // MAC地址
#define AUTH_KEY        "4af0435dff3f42b5ad773a664e4bcaf8"  // 认证密钥
#define DEVICE_ID       "f1952f11"  // 设备ID

// #define PRODUCT_KEY     "a54283350726462daaeab498ffee87de"  // 产品密钥
// #define PRODUCT_SECRET  "697d826fcc2642fbae7949683e3cf8e0"  // 产品密钥
// #define PRODUCT_SECRET  "697d826fcc2642fbae7949683e3cf8e0"  // 产品密钥
// #define TEST_MAC        "d83bda4cda74"  // MAC地址
// #define TEST_USER     "6499e3651fd94dc6926a4103391e9b7e"  // MAC地址
// #define AUTH_KEY        "f9efa5e4242e42e0a6346cca2a3b9fdf"  // 认证密钥
// #define DEVICE_ID       "f79a2d75"  // 设备ID

// #define PRODUCT_KEY     "a54283350726462daaeab498ffee87de"  // 产品密钥
// #define PRODUCT_SECRET  "697d826fcc2642fbae7949683e3cf8e0"  // 产品密钥

// // #define HARDCODE_INFO_ENABLE        1
// #define TEST_MAC        "d83bda4c7777"  // MAC地址
// #define AUTH_KEY        "c440d6b807f74a6694aad8838420bb84"  // 认证密钥
// #define DEVICE_ID       "nbbb107b"  // 设备ID

#define MALLOC(size)  heap_caps_malloc(size,MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT)
#define FREE(ptr)     do {heap_caps_free(ptr);ptr = NULL;} while (0)
#define hexdump(pName, buf, len) do { \
    if (pName) { \
        printf("%s: ", pName); \
    } \
    for (size_t i = 0; i < len; i++) { \
        printf("%02X ", ((const uint8_t *)(buf))[i]); \
    } \
    printf("\n"); \
} while (0)
/**
 * @brief 获取产品信息
 * @return 产品信息指针，失败返回NULL
 */
product_info_t* get_product_info(void);

// 在头文件中导出结构体定义
typedef struct {
    char bot_id[64];
    char voice_id[64];
    char user_id[64];
    char conv_id[64];
    char access_token[256];
    char workflow_id[64];
    int expires_in;
    cJSON *config;  // Pointer to cJSON object for coze_websocket.config
} rtc_params_t;

#endif /* _CONFIG_H_ */
