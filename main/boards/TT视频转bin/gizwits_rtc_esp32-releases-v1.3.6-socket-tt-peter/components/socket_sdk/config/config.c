
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "config/config.h"
#include "esp_log.h"
// #include "tool.h"

static const char *TAG = "CONFIG";

// 产品信息实例
static product_info_t product_info = {0};
static bool product_info_initialized = false;

/**
 * @brief 初始化产品信息
 * @return true成功，false失败
 */
static bool init_product_info(void)
{
    if (product_info_initialized) {
        return true;
    }

    // 从config.h获取产品信息
    storage_get_cached_auth_config(&product_info.szDID, &product_info.szAuthKey, product_info.szPK, product_info.szPKs);
    
    // 获取MAC地址
    if (!get_mac_str(product_info.szMac, false)) {
        ESP_LOGE(TAG, "Failed to get MAC address");
        return false;
    }

    // 生成设备ID (可以根据需求修改生成规则)
    snprintf(product_info.szDID, sizeof(product_info.szDID) - 1,
             "%s_%s", product_info.szPK, product_info.szMac);

    product_info_initialized = true;
    ESP_LOGI(TAG, "Product info initialized: PK=%s, DID=%s",
             product_info.szPK, product_info.szDID);
             
    return true;
}

static product_info_t gatProductInfo;

product_info_t* get_product_info(void)
{
    product_info_t *pInfo = NULL;
    pInfo = (product_info_t*)&gatProductInfo;
    storage_get_cached_auth_config(&pInfo->szDID, &pInfo->szAuthKey, pInfo->szPK, pInfo->szPKs);
    return pInfo;
}
