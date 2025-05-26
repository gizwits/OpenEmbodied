#include "auth.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>
#include "settings.h"

static const char *TAG = "Auth";

// 验证数据段的辅助函数
static bool verify_data_segment(const char* data, size_t len, 
                              char* auth_key, char* did, char* pk, char* ps) {
    if (!data || !auth_key || !did || !pk || !ps) return false;
    
    // 尝试解析数据
    if (sscanf(data, "%32[^,],%8[^,],%32[^,],%32[^;]", 
               auth_key, did, pk, ps) == 4) {
        return true;
    }
    return false;
}

// 从分区读取数据的辅助函数
static esp_err_t read_nvs_data(const char* partition_name, size_t offset, 
                             void* buffer, size_t size) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, partition_name);
    
    if (!partition) {
        ESP_LOGE(TAG, "Partition %s not found", partition_name);
        return ESP_ERR_NOT_FOUND;
    }
    
    return esp_partition_read(partition, offset, buffer, size);
}

void Auth::init() {
    uint8_t buffer[256] = {0};  // 假设数据不会超过256字节
    esp_err_t err;
    
    // 从auth分区读取数据
    err = read_nvs_data("auth", 0, buffer, sizeof(buffer));

    Settings settings("wifi", true);

    if (err == ESP_OK) {
        char auth_key[33] = {0};
        char did[9] = {0};
        char pk[33] = {0};
        char ps[33] = {0};
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
            ESP_LOGI(TAG, "Auth data read successfully");
            m_auth_key = auth_key;
            m_device_id = did;
            
            // 检查 product key 和 secret 是否为空
            if (strlen(pk) > 0 && strlen(ps) > 0) {
                m_product_key = pk;
                m_product_secret = ps;
            } else {
                ESP_LOGW(TAG, "Product key or secret is empty, using default values");
                m_product_key = CONFIG_PRODUCT_KEY;
                m_product_secret = CONFIG_PRODUCT_SECRET;
            }
            
            m_is_initialized = true;
        } else {
            ESP_LOGW(TAG, "Failed to parse auth data, using default values");
            m_product_key = CONFIG_PRODUCT_KEY;
            m_product_secret = CONFIG_PRODUCT_SECRET;
            m_device_id = settings.GetString("did", "");

            ESP_LOGI(TAG, "Device ID: %s", m_device_id.c_str());
            m_is_initialized = true;
        }
    } else {
        ESP_LOGE(TAG, "Failed to read auth data, using default values");
        m_product_key = CONFIG_PRODUCT_KEY;
        m_product_secret = CONFIG_PRODUCT_SECRET;
        m_device_id = settings.GetString("did", "");
        m_is_initialized = true;
    }
}

std::string Auth::getAuthKey() {
    return m_auth_key;
}

std::string Auth::getDeviceId() {
    return m_device_id;
}

std::string Auth::getProductKey() {
    return m_product_key;
}

std::string Auth::getProductSecret() {
    return m_product_secret;
}
