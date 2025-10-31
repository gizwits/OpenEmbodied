#ifndef _STORAGE_H_
#define _STORAGE_H_

#include <stdbool.h>
#include <esp_err.h>
#include "esp_wifi.h"

// 分区定义
#define WIFI_CONFIG_PARTITION "config_data"
#define WIFI_CONFIG_NAMESPACE "wifi_config"
#define AUTH_CONFIG_PARTITION "auth_data"
#define AUTH_CONFIG_NAMESPACE "auth_config"

// // 默认值定义
// #define AUTH_KEY        "f9efa5e4242e42e0a6346cca2a3b9fdf"  // 认证密钥
// #define DEVICE_ID       "f79a2d75"  // 设备ID


// 产测模式
typedef enum {
    FACTORY_TEST_MODE_NONE = 0,
    FACTORY_TEST_MODE_IN_FACTORY,
    FACTORY_TEST_MODE_AGING,
    FACTORY_TEST_MODE_AGING_FINISHED,
    FACTORY_TEST_MODE_MAX
} factory_test_mode_t;


// WiFi相关函数
esp_err_t storage_save_wifi_config(const char *ssid, const char *password);
bool storage_load_wifi_config(char *ssid, char *password);
esp_err_t storage_clear_wifi_config(void);
void storage_reset_wifi_config(void);
bool storage_has_wifi_config(void);

// Bootstrap相关函数
esp_err_t storage_save_bootstrap(bool bootstrap);
bool storage_load_bootstrap(void);
void storage_clear_bootstrap(void);
bool storage_load_bootstrap_cache(void);

// UID相关函数
esp_err_t storage_save_uid(const char *uid);
bool storage_load_uid(char *uid);

// 认证相关函数
esp_err_t storage_save_auth_config(const char *did, const char *auth_key);
bool storage_get_cached_auth_config(char *did, char *auth_key, char *pk, char *ps);
esp_err_t storage_clear_auth_config(void);

// 初始化函数
esp_err_t storage_init(void);
// 检查授权是否有效
bool storage_is_auth_valid(void);

// 产测相关函数
esp_err_t storage_save_factory_test_mode(int mode);
bool storage_load_factory_test_mode(int *mode);

#endif /* _STORAGE_H_ */ 