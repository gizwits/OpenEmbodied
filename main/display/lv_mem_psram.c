/**
 * @file lv_mem_psram.c
 * @brief Custom memory allocation functions for LVGL using PSRAM
 * 
 * When CONFIG_LV_USE_CUSTOM_MALLOC is set, LVGL expects these functions
 * to be implemented externally.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stddef.h>
#include <string.h>

static const char* TAG = "lv_mem_psram";

/**
 * Initialize memory module (required by LVGL)
 */
void lv_mem_init(void) {
    // 打印内存状态
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    ESP_LOGI(TAG, "LVGL memory init - PSRAM free: %zu KB, Internal free: %zu KB", 
             psram_free / 1024, internal_free / 1024);
             
    // 如果 PSRAM 太少，警告
    if (psram_free < 1024 * 1024) {  // 少于 1MB
        ESP_LOGW(TAG, "Low PSRAM available: %zu KB", psram_free / 1024);
    }
}

/**
 * Deinitialize memory module (required by LVGL)
 */
void lv_mem_deinit(void) {
    // No special cleanup needed for heap_caps
}

/**
 * Core allocation function that LVGL calls
 * This allocates from PSRAM preferentially
 */
void* lv_malloc_core(size_t size) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (ptr == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes", size);
    }
    return ptr;
}

/**
 * Core free function that LVGL calls
 */
void lv_free_core(void* ptr) {
    heap_caps_free(ptr);
}

/**
 * Core realloc function that LVGL calls
 */
void* lv_realloc_core(void* ptr, size_t size) {
    if (ptr == NULL) {
        // 如果 ptr 为 NULL，相当于 malloc
        return lv_malloc_core(size);
    }
    
    if (size == 0) {
        // 如果 size 为 0，相当于 free
        lv_free_core(ptr);
        return NULL;
    }
    
    // 使用默认的 realloc，让系统自动处理内存位置
    // ESP-IDF 的 heap_caps_realloc 会保持原有的内存类型
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    
    if (new_ptr == NULL) {
        ESP_LOGE(TAG, "Failed to realloc %zu bytes", size);
    }
    
    return new_ptr;
}