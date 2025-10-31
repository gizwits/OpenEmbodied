#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "storage.h"
#include "storage_internal.h"
#include "config.h"
#include <zlib.h>  // 添加头文件，用于crc32计算

static const char *TAG = "STORAGE";

// 认证缓存
static auth_cache_t g_auth_cache = {
    .is_valid = false
};

// NVS Keys
#define KEY_SSID "ssid"
#define KEY_PASSWORD "password"
#define KEY_DID "did"
#define KEY_BOOTSTRAP "bootstrap"
#define KEY_UID "uid"
#define KEY_AUTH "auth_key"
#define KEY_FACTORY_TEST "factory_test"

#define STORAGE_NAMESPACE "local_data"
#define VOLUME_KEY "volume"

#define KEY_ONBOARDING_FLAG "onboarding_flag"

bool bootstrap_cache = false;

esp_err_t read_nvs_data(const char* partition_name, size_t offset, uint8_t* data, size_t length) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_NVS,
        partition_name
    );
    
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find NVS partition: %s", partition_name);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_read(partition, offset, data, length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read data from partition: %d", err);
        return err;
    }

    return ESP_OK;
}


auth_cache_t* storage_get_auth_cache(void)
{
    return &g_auth_cache;
}

esp_err_t storage_open_handle(const char *partition, const char *namespace,
                            nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Opening NVS handle - partition: %s, namespace: %s", partition, namespace);
    
    // 先检查分区是否存在
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, partition);
    if (!part) {
        ESP_LOGE(TAG, "Partition %s not found!", partition);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Found partition %s at address 0x%x", partition, part->address);

    esp_err_t ret = nvs_open_from_partition(partition, namespace, mode, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s (0x%x)", esp_err_to_name(ret), ret);
        
        // 如果是 NOT_FOUND 错误，尝试读取分区内容
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            uint8_t buffer[16];
            if (esp_partition_read(part, 0, buffer, sizeof(buffer)) == ESP_OK) {
                // ESP_LOGI(TAG, "First 16 bytes of partition:");
                // for (int i = 0; i < 16; i++) {
                //     printf("%02x ", buffer[i]);
                // }
                // printf("\n");
            }
        }
    }
    return ret;
}

esp_err_t storage_save_string(nvs_handle_t handle, const char *key, const char *value)
{
    if (!key || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_set_str(handle, key, value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving %s: %s", key, esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t storage_load_string(nvs_handle_t handle, const char *key, char *value, size_t *length)
{
    if (!key || !value || !length) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = nvs_get_str(handle, key, value, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error loading %s: %s", key, esp_err_to_name(ret));
    }

    return ret;
}

static bool is_wifi_config_loaded = false;
static bool is_wifi_config_valid = false;

// WiFi配置相关函数
esp_err_t storage_save_wifi_config(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = storage_save_string(handle, KEY_SSID, ssid);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }

    ret = storage_save_string(handle, KEY_PASSWORD, password);

    is_wifi_config_loaded = true;
    is_wifi_config_valid = true;
    
    nvs_close(handle);
    ESP_LOGI(TAG, "storage_save_wifi_config: SSID=%s done", ssid);
    return ret;
}

bool storage_load_wifi_config(char *ssid, char *password)
{
    if (!ssid || !password) {
        return false;
    }

    nvs_handle_t handle;
    if (storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t ssid_len = 32;
    size_t pass_len = 64;

    bool success = (storage_load_string(handle, KEY_SSID, ssid, &ssid_len) == ESP_OK) &&
                  (storage_load_string(handle, KEY_PASSWORD, password, &pass_len) == ESP_OK);

    is_wifi_config_loaded = true;
    is_wifi_config_valid = success;

    nvs_close(handle);
    return success;
}

bool storage_has_wifi_config(void)
{
    char ssid[33] = {0};
    char password[65] = {0};

    if (is_wifi_config_loaded) {
        return is_wifi_config_valid;
    }

    if (storage_load_wifi_config(ssid, password) == false) {
        return false;
    }
    return true;
}


esp_err_t storage_clear_wifi_config(void)
{
    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    is_wifi_config_loaded = true;
    is_wifi_config_valid = false;
    
    nvs_close(handle);
    return ret;
}

// Bootstrap相关函数
esp_err_t storage_save_bootstrap(bool bootstrap)
{
    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(handle, KEY_BOOTSTRAP, bootstrap ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    bootstrap_cache = bootstrap;
    return ret;
}

bool storage_load_bootstrap_cache(void)
{
    return bootstrap_cache;
}

bool storage_load_bootstrap(void)
{
    nvs_handle_t handle;
    if (storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t bootstrap = 0;
    esp_err_t ret = nvs_get_u8(handle, KEY_BOOTSTRAP, &bootstrap);
    nvs_close(handle);

    ESP_LOGE(TAG, "storage_load_bootstrap: %d", bootstrap);
    bootstrap_cache = (ret == ESP_OK && bootstrap == 1);
    return bootstrap_cache;
}

void storage_clear_bootstrap(void)
{
    storage_save_bootstrap(false);
    storage_save_factory_test_mode(FACTORY_TEST_MODE_NONE);
}

// UID相关函数
esp_err_t storage_save_uid(const char *uid)
{
    if (!uid) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = storage_save_string(handle, KEY_UID, uid);
    nvs_close(handle);
    return ret;
}

bool storage_load_uid(char *uid)
{
    if (!uid) {
        return false;
    }

    nvs_handle_t handle;
    if (storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t uid_len = 33;
    esp_err_t ret = storage_load_string(handle, KEY_UID, uid, &uid_len);
    nvs_close(handle);

    return (ret == ESP_OK);
}

// 认证相关函数
esp_err_t storage_save_auth_config(const char *did, const char *auth_key)
{
    if (!did || !auth_key) {
        return ESP_ERR_INVALID_ARG;
    }

    // 更新缓存
    strncpy(g_auth_cache.did, did, sizeof(g_auth_cache.did) - 1);
    strncpy(g_auth_cache.auth_key, auth_key, sizeof(g_auth_cache.auth_key) - 1);
    g_auth_cache.is_valid = true;

    // 保存到存储
    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(AUTH_CONFIG_PARTITION, AUTH_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = storage_save_string(handle, KEY_DID, did);
    if (ret == ESP_OK) {
        ret = storage_save_string(handle, KEY_AUTH, auth_key);
    }

    nvs_close(handle);
    return ret;
}


bool storage_get_cached_auth_config(char *did, char *auth_key, char *pk, char *ps)
{
#if !defined(HARDCODE_INFO_ENABLE)
    if (!did || !auth_key || !pk || !ps || !g_auth_cache.is_valid) {
        return false;
    }

    strncpy(did, g_auth_cache.did, 8);
    strncpy(auth_key, g_auth_cache.auth_key, 32);
    
    // 如果pk为空，使用默认值
    if (g_auth_cache.pk[0] == '\0') {
        strncpy(pk, PRODUCT_KEY, 32);
    } else {
        strncpy(pk, g_auth_cache.pk, 32);
    }
    
    // 如果ps为空，使用默认值
    if (g_auth_cache.ps[0] == '\0') {
        strncpy(ps, PRODUCT_SECRET, 32);
    } else {
        strncpy(ps, g_auth_cache.ps, 32);
    }
#else
#warning "HARDCODE_INFO_ENABLE is defined, using hardcode info!"
    if (!did || !auth_key || !pk || !ps) {
        ESP_LOGW(TAG, "hardcode params is not NULL!");
        return false;
    }

    // ESP_LOGI(TAG, "hardcode info: did: %s, auth_key: %s, pk: %s, ps: %s", did, auth_key, pk, ps);

    // hardcode info
    strncpy(did, DEVICE_ID, 8);
    strncpy(auth_key, AUTH_KEY, 32);
    strncpy(pk, PRODUCT_KEY, 32);
    strncpy(ps, PRODUCT_SECRET, 32);
#endif
    return true;
}

esp_err_t storage_clear_auth_config(void)
{
    // 清除缓存
    memset(&g_auth_cache, 0, sizeof(g_auth_cache));
    g_auth_cache.is_valid = false;

    // 清除存储
    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(AUTH_CONFIG_PARTITION, AUTH_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

void storage_reset_wifi_config(void)
{
    storage_clear_wifi_config();
    storage_clear_bootstrap();
}

// 添加辅助函数用于验证数据段
static bool verify_data_segment(const char* data, size_t max_len, 
                              char* out_auth_key, char* out_did,
                              char* out_pk, char* out_ps) {
    if (!data || !out_auth_key || !out_did || !out_pk || !out_ps || 
        max_len < 43) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    char temp_buffer[256] = {0};
    char *auth_key, *did, *pk = NULL, *ps = NULL, *crc;
    uint32_t stored_checksum, calculated_checksum;
    
    // 安全复制数据
    size_t copy_len = (max_len < sizeof(temp_buffer) - 1) ? max_len : sizeof(temp_buffer) - 1;
    memcpy(temp_buffer, data, copy_len);
    temp_buffer[sizeof(temp_buffer) - 1] = '\0';
    
    // 分割数据
    auth_key = temp_buffer;
    
    // 找到第一个逗号 - auth_key和did的分隔符
    did = strchr(auth_key, ',');
    if (!did) {
        ESP_LOGE(TAG, "First comma not found");
        return false;
    }
    *did = '\0';
    did++;
    
    // 找到下一个逗号 - 可能是did和pk的分隔符，或者是did和checksum的分隔符
    crc = strchr(did, ',');
    if (!crc) {
        ESP_LOGE(TAG, "Next comma not found");
        return false;
    }
    
    // 检查是否是长格式（包含pk和ps）
    char* next_comma = strchr(crc + 1, ',');
    if (next_comma) {  // 长格式
        *crc = '\0';
        pk = crc + 1;
        
        // 找到pk和ps的分隔符
        ps = strchr(pk, ',');
        if (!ps) {
            ESP_LOGE(TAG, "PS separator not found");
            return false;
        }
        *ps = '\0';
        ps++;
        
        // 找到ps和checksum的分隔符
        crc = strchr(ps, ',');
        if (!crc) {
            ESP_LOGE(TAG, "Checksum separator not found");
            return false;
        }
        *crc = '\0';
        crc++;
    } else {  // 短格式
        *crc = '\0';
        crc++;
        // pk和ps保持为NULL
    }
    
    // 如果有分号，截断它
    char* end = strchr(crc, ';');
    if (end) {
        *end = '\0';
    }
    
    // 验证长度
    size_t auth_key_len = strlen(auth_key);
    size_t did_len = strlen(did);
    
    if (auth_key_len != 32 || did_len != 8) {
        ESP_LOGE(TAG, "Invalid length - auth_key: %d, did: %d", 
                 auth_key_len, did_len);
        return false;
    }
    
    // 获取存储的校验和
    char *endptr;
    stored_checksum = strtoul(crc, &endptr, 10);
    if (crc == endptr) {
        ESP_LOGE(TAG, "Invalid checksum format");
        return false;
    }
    
    // 计算实际校验和
    char verify_buf[256] = {0};
    if (pk && ps) {
        snprintf(verify_buf, sizeof(verify_buf), "%s,%s,%s,%s", 
                 auth_key, did, pk, ps);
    } else {
        snprintf(verify_buf, sizeof(verify_buf), "%s,%s", 
                 auth_key, did);
    }
    
    // 计算校验和
    calculated_checksum = 0;
    const uint8_t* p = (const uint8_t*)verify_buf;
    size_t len = strlen(verify_buf);
    
    for (size_t i = 0; i < len; i++) {
        calculated_checksum += p[i];
    }
    
    if (stored_checksum != calculated_checksum) {
        // ESP_LOGE(TAG, "Checksum mismatch: stored=%u, calculated=%u", 
        //          stored_checksum, calculated_checksum);
        return false;
    }
    
    // 安全复制结果
    memset(out_auth_key, 0, 33);
    memset(out_did, 0, 9);
    memset(out_pk, 0, 33);
    memset(out_ps, 0, 33);
    
    strncpy(out_auth_key, auth_key, 32);
    strncpy(out_did, did, 8);
    
    if (pk && ps) {
        strncpy(out_pk, pk, 32);
        strncpy(out_ps, ps, 32);
    }
    // 如果pk和ps为NULL，对应的输出缓冲区会保持为空字符串
    
    ESP_LOGI(TAG, "Verification successful");
    ESP_LOGI(TAG, "auth_key: %s", auth_key);
    ESP_LOGI(TAG, "did: %s", did);
    if (pk && ps) {
        ESP_LOGI(TAG, "pk: %s", pk);
        ESP_LOGI(TAG, "ps: %s", ps);
    }
    ESP_LOGI(TAG, "crc: %u", stored_checksum);
    
    return true;
}

esp_err_t storage_init(void)
{
    // 检查分区表中是否存在 auth_data 分区
    const esp_partition_t* auth_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, AUTH_CONFIG_PARTITION);
    
    if (auth_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find auth_data partition!");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Found auth_data partition at offset 0x%x with size %d", 
             auth_partition->address, auth_partition->size);

    // 初始化 WiFi 配置分区
    esp_err_t ret = nvs_flash_init_partition(WIFI_CONFIG_PARTITION);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        const esp_partition_t* config_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, WIFI_CONFIG_PARTITION);
        if (config_partition) {
            ESP_ERROR_CHECK(esp_partition_erase_range(config_partition, 0, config_partition->size));
        }
        ret = nvs_flash_init_partition(WIFI_CONFIG_PARTITION);
    }
    ESP_LOGI(TAG, "WiFi config partition init result: %d", ret);
    uint8_t buffer[128];  // 增大缓冲区以容纳两段数据
    esp_err_t err;
    
    // 从auth_data分区读取数据
    err = read_nvs_data(AUTH_CONFIG_PARTITION, 0, buffer, sizeof(buffer));
    if (err == ESP_OK) {
        // ESP_LOGI(TAG, "Read data successfully");
        // // 打印buffer
        // ESP_LOGI(TAG, "Buffer as hex:");
        // for (int i = 0; i < sizeof(buffer); i++) {
        //     printf("%02x ", buffer[i]);
        //     if ((i + 1) % 16 == 0) printf("\n");
        // }
        // printf("\n");
        
        // ESP_LOGI(TAG, "Buffer as string: %s", (char *)buffer);
        
        char auth_key[33] = {0};
        char did[9] = {0};
        char pk[33] = {0};  // 根据实际长度调整
        char ps[33] = {0};  // 根据实际长度调整
        bool success = false;
        
        // 尝试验证第一段数据
        success = verify_data_segment((char*)buffer, sizeof(buffer), 
                                    auth_key, did, pk, ps);
        
        // 如果第一段失败，尝试第二段
        if (!success) {
            char* second_segment = strchr((char*)buffer, ';');
            if (second_segment) {
                second_segment++;
                success = verify_data_segment(second_segment, 
                    sizeof(buffer) - (second_segment - (char*)buffer), 
                    auth_key, did, pk, ps);
            }
        }
        
        if (success) {
            printf("\n\n-----------auth successful------------\n\n");
            strncpy(g_auth_cache.auth_key, auth_key, sizeof(g_auth_cache.auth_key) - 1);
            strncpy(g_auth_cache.did, did, sizeof(g_auth_cache.did) - 1);
            strncpy(g_auth_cache.pk, pk, sizeof(g_auth_cache.pk) - 1);
            strncpy(g_auth_cache.ps, ps, sizeof(g_auth_cache.ps) - 1);
            g_auth_cache.is_valid = true;
            
            // ESP_LOGE(TAG, "Parsed auth_key: %s", g_auth_cache.auth_key);
            // ESP_LOGE(TAG, "Parsed did: %s", g_auth_cache.did);
            // ESP_LOGE(TAG, "Parsed pk: %s", g_auth_cache.pk);
            // ESP_LOGE(TAG, "Parsed ps: %s", g_auth_cache.ps);
        } else {
            // 使用默认值
            strncpy(g_auth_cache.did, DEVICE_ID, sizeof(g_auth_cache.did) - 1);
            strncpy(g_auth_cache.auth_key, AUTH_KEY, sizeof(g_auth_cache.auth_key) - 1);
            strncpy(g_auth_cache.pk, PRODUCT_KEY, sizeof(g_auth_cache.pk) - 1);
            strncpy(g_auth_cache.ps, PRODUCT_SECRET, sizeof(g_auth_cache.ps) - 1);
            g_auth_cache.is_valid = true;
        }

        
    } else {
        printf("\n\n-----------auth failed------------\n\n");
        ESP_LOGE(TAG, "Failed to read auth data, using default values");
        strncpy(g_auth_cache.did, DEVICE_ID, sizeof(g_auth_cache.did) - 1);
        strncpy(g_auth_cache.auth_key, AUTH_KEY, sizeof(g_auth_cache.auth_key) - 1);
        strncpy(g_auth_cache.pk, PRODUCT_KEY, sizeof(g_auth_cache.pk) - 1);
        strncpy(g_auth_cache.ps, PRODUCT_SECRET, sizeof(g_auth_cache.ps) - 1);
        g_auth_cache.is_valid = true;
    }
    
    return ret;
}

// 检查授权是否有效
bool storage_is_auth_valid(void)
{
    return g_auth_cache.is_valid;
}

// 保存产测模式
esp_err_t storage_save_factory_test_mode(int mode)
{
    char mode_str[10] = {0};
    if (mode < FACTORY_TEST_MODE_NONE || mode >= FACTORY_TEST_MODE_MAX) {
        ESP_LOGE(TAG, "Invalid factory test mode: %d", mode);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open factory test mode handle");
        return ret;
    }

    snprintf(mode_str, sizeof(mode_str) - 1, "%d", mode);
    mode_str[sizeof(mode_str) - 1] = '\0';
    ESP_LOGW(TAG, "save factory test mode: %s", mode_str);
    ret = storage_save_string(handle, KEY_FACTORY_TEST, mode_str);
    nvs_close(handle);
    ESP_LOGW(TAG, "save factory test mode result: %d", ret);
    return ret;
}

// 加载产测模式
bool storage_load_factory_test_mode(int *mode)
{
    char mode_str[10] = {0};
    if (!mode) {
        ESP_LOGE(TAG, "Invalid mode ptr");
        return false;
    }

    // 初始化产测模式
    *mode = 0;

    nvs_handle_t handle;
    if (storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open factory test mode handle");
        return false;
    }

    size_t mode_len = sizeof(mode_str);
    esp_err_t ret = storage_load_string(handle, KEY_FACTORY_TEST, mode_str, &mode_len);
    nvs_close(handle);
    ESP_LOGI(TAG, "load factory test mode: %s", mode_str);

    int mode_value = atoi(mode_str);
    if (mode_value >= FACTORY_TEST_MODE_NONE && mode_value < FACTORY_TEST_MODE_MAX) {
        *mode = mode_value;
        ESP_LOGI(TAG, "Factory test mode: %d", *mode);
    } else {
        ESP_LOGE(TAG, "Invalid factory test mode: %d", mode_value);
        *mode = FACTORY_TEST_MODE_NONE;
    }
    ESP_LOGI(TAG, "converted factory test mode: %d", *mode);

    return (ret == ESP_OK);
}

// 保存配置模式标志
esp_err_t storage_save_onboarding_flag(bool onboarding_flag)
{
    nvs_handle_t handle;
    esp_err_t ret = storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open onboarding flag handle");
        return ret;
    }

    uint8_t flag = onboarding_flag ? 1 : 0;
    ESP_LOGI(TAG, "save onboarding flag: %d", flag);
    ret = nvs_set_u8(handle, KEY_ONBOARDING_FLAG, flag);
    nvs_close(handle);
    ESP_LOGI(TAG, "save onboarding flag result: %d", ret);
    return ret;
}

// 加载配置模式标志
bool storage_load_onboarding_flag(bool *onboarding_flag)
{
    if (!onboarding_flag) {
        ESP_LOGE(TAG, "Invalid onboarding flag ptr");
        return false;
    }

    nvs_handle_t handle;
    if (storage_open_handle(WIFI_CONFIG_PARTITION, WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open onboarding flag handle");
        return false;
    }

    uint8_t flag = 0;
    esp_err_t ret = nvs_get_u8(handle, KEY_ONBOARDING_FLAG, &flag);
    nvs_close(handle);
    ESP_LOGI(TAG, "load onboarding flag: %d", flag);

    *onboarding_flag = (flag == 1);
    return (ret == ESP_OK);
}

