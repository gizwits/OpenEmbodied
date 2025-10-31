#include "tool.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "config/config.h"
static const char *TAG = "TOOL";

bool generate_random_string(uint8_t len, char *out_buf, size_t buf_size)
{
    if (!out_buf || buf_size <= len) {
        return false;
    }
    
    for(uint8_t i = 0; i < len; i++) {
        out_buf[i] = 'A' + (esp_random() % 26);  // A-Z
    }
    out_buf[len] = '\0';
    
    ESP_LOGD(TAG, "Generated random string: %s", out_buf);
    return true;
}

int hex_to_str(char *dest, const uint8_t *src, size_t src_len, bool uppercase)
{
    if (!dest || !src) {
        return -1;
    }
    
    for(size_t i = 0; i < src_len; i++) {
        sprintf(dest + (i * 2), 
                uppercase ? "%02X" : "%02x", 
                src[i]);
    }
    dest[src_len * 2] = '\0';
    
    return src_len * 2;
}

int str_to_hex(uint8_t *dest, const char *src, size_t dest_len)
{
    if (!dest || !src || strlen(src) / 2 > dest_len) {
        return -1;
    }
    
    size_t len = strlen(src) / 2;
    for(size_t i = 0; i < len; i++) {
        sscanf(src + (i * 2), "%2hhx", &dest[i]);
    }
    
    return len;
}

bool get_mac_str(char *mac_str, bool uppercase)
{
    uint8_t mac[6];
    
#if !defined(HARDCODE_INFO_ENABLE)
    // if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to get MAC address");
    //     return false;
    // }
    
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret == ESP_OK) {
        // ESP_LOGI("MAC", "WiFi STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
        //          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGE("MAC", "Failed to read MAC address");
        return false;
    }

    hex_to_str(mac_str, mac, 6, uppercase);
    ESP_LOGD(TAG, "Device MAC: %s", mac_str);
    return true;
#else
#warning "HARDCODE_INFO_ENABLE is defined, using hardcode mac!"
    strncpy(mac_str, TEST_MAC, MAC_LEN);
    ESP_LOGI(TAG, "hardcode mac: %s", mac_str);
    return true;
#endif

}

void print_heap_info(const char *tag, const char *note)
{
    if (!tag) {
        tag = "HEAP";
    }

    // 获取堆信息
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    // 避免除零错误
    uint32_t total_size = info.total_free_bytes + info.total_allocated_bytes;
    if (total_size == 0) {
        ESP_LOGE(tag, "Invalid heap info (total size is 0)");
        return;
    }

    // 计算使用率
    float usage_percent = (info.total_allocated_bytes * 100.0f) / total_size;

    // 打印堆信息
    ESP_LOGE(tag, "Heap Info%s%s:", 
             note ? " (" : "",
             note ? note : "");
    ESP_LOGE(tag, "  Total: %u bytes,  Free: %u bytes (%u blocks),  Used: %u bytes (%u blocks),  Usage: %.1f%%", 
             total_size, info.total_free_bytes, info.free_blocks,
             info.total_allocated_bytes, info.allocated_blocks, usage_percent);
    
    // 如果碎片化严重，打印警告
    if (info.free_blocks > 10 && info.free_blocks > info.allocated_blocks) {
        ESP_LOGE(tag, "  High fragmentation: %u free blocks!", info.free_blocks);
    }

    // 检查最小空闲内存
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGE(tag, "  Minimum free ever: %u bytes", min_free);
}