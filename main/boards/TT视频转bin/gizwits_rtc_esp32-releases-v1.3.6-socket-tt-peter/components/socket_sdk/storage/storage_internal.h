#ifndef _STORAGE_INTERNAL_H_
#define _STORAGE_INTERNAL_H_

#include <esp_err.h>
#include "nvs_flash.h"

// 缓存结构体
typedef struct {
    char did[9];
    char auth_key[33];
    char pk[33];
    char ps[33];
    bool is_valid;
} auth_cache_t;

// 通用存储函数声明
esp_err_t storage_open_handle(const char *partition, const char *namespace, 
                            nvs_open_mode_t mode, nvs_handle_t *handle);
esp_err_t storage_save_string(nvs_handle_t handle, const char *key, const char *value);
esp_err_t storage_load_string(nvs_handle_t handle, const char *key, char *value, size_t *length);

// 获取认证缓存的函数
auth_cache_t* storage_get_auth_cache(void);

#endif /* _STORAGE_INTERNAL_H_ */ 