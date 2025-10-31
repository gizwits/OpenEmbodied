#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "crash_handler.h"
#include "error_monitor.h"
#include "unit/tool.h"
#include <mbedtls/base64.h>

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
#include "esp_rom_spiflash.h"
#define SPI_READ esp_rom_spiflash_read
#else
#include "esp_spi_flash.h"
#define SPI_READ spi_flash_read
#endif

static const char *TAG = "CRASH_HANDLER";
#define CRASH_LOG_NAMESPACE "local_data"
#define CRASH_COUNT_KEY "crash_count"
#define CRASH_LOG_KEY_PREFIX "crash_log_"
#define MAX_CRASH_LOG_SIZE 1024

// 崩溃计数器
static uint32_t crash_count = 0;

// 从核心转储分区读取数据
static bool coredump_read(uint32_t **des, size_t *len)
{
    size_t addr = 0;
    
    // 获取核心转储数据地址和大小
    if (esp_core_dump_image_get(&addr, len) != ESP_OK) {
        ESP_LOGW(TAG, "No dump info to upload");
        return false;
    }
    
    if (*len == 0) {
        ESP_LOGI(TAG, "Core dump size is 0");
        return false;
    }
    
    ESP_LOGI(TAG, "Found core dump at address 0x%x, size %d bytes", (unsigned int)addr, *len);
    
    // 分配内存存储核心转储数据
    *des = malloc(*len);
    if (*des == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for core dump");
        *len = 0;
        return false;
    }
    
    // 读取核心转储数据
    if (SPI_READ(addr, *des, *len) != ESP_OK) {
        ESP_LOGE(TAG, "Core dump read ERROR");
        free(*des);
        *des = NULL;
        *len = 0;
        return false;
    }
    
    ESP_LOGI(TAG, "Core dump read successfully");
    return true;
}

// 检查是否需要上传核心转储
static bool coredump_need_upload()
{
    bool ret = false;
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason is %d", reset_reason);
    
    switch (reset_reason) {
        case ESP_RST_UNKNOWN:
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        // case ESP_RST_WDT:  // 在某些版本的ESP-IDF中不存在
            ret = true;
            break;
        default:
            ret = false;
    }
    
    return ret;
}

// 从核心转储数据中提取崩溃信息
static bool extract_crash_info(uint32_t *data, size_t len, char *info_buf, size_t buf_size)
{
    if (!data || !info_buf || buf_size == 0) {
        return false;
    }
    
    // 将核心转储数据视为字符串，尝试查找关键信息
    char *data_str = (char *)data;
    
    // 查找常见的崩溃信息标记
    char *panic_str = strstr(data_str, "Guru Meditation Error");
    char *backtrace_str = strstr(data_str, "Backtrace:");
    char *register_str = strstr(data_str, "register dump");
    char *abort_str = strstr(data_str, "abort() was called");
    char *exception_str = strstr(data_str, "Exception ");
    char *stack_str = strstr(data_str, "stack overflow");
    
    // 构建基本信息
    snprintf(info_buf, buf_size, "Core dump size: %d bytes", len);
    
    // 添加崩溃原因
    if (panic_str) {
        char *end = strchr(panic_str, '\n');
        if (end) {
            size_t panic_len = end - panic_str;
            strncat(info_buf, "\nReason: ", buf_size - strlen(info_buf) - 1);
            strncat(info_buf, panic_str, MIN(panic_len, buf_size - strlen(info_buf) - 1));
        }
    } else if (abort_str) {
        strncat(info_buf, "\nReason: Program aborted", buf_size - strlen(info_buf) - 1);
    } else if (exception_str) {
        char *end = strchr(exception_str, '\n');
        if (end) {
            size_t exc_len = end - exception_str;
            strncat(info_buf, "\nReason: ", buf_size - strlen(info_buf) - 1);
            strncat(info_buf, exception_str, MIN(exc_len, buf_size - strlen(info_buf) - 1));
        }
    } else if (stack_str) {
        strncat(info_buf, "\nReason: Stack overflow detected", buf_size - strlen(info_buf) - 1);
    }
    
    // 添加寄存器信息
    if (register_str) {
        char *end = strstr(register_str, "\n\n");
        if (!end) {
            end = strchr(register_str, '\n');
        }
        
        if (end) {
            size_t reg_len = end - register_str;
            strncat(info_buf, "\nRegisters: ", buf_size - strlen(info_buf) - 1);
            strncat(info_buf, register_str, MIN(reg_len, buf_size - strlen(info_buf) - 1));
        }
    }
    
    // 添加回溯信息
    if (backtrace_str) {
        char *end = strchr(backtrace_str, '\n');
        if (end) {
            size_t bt_len = end - backtrace_str;
            strncat(info_buf, "\n", buf_size - strlen(info_buf) - 1);
            strncat(info_buf, backtrace_str, MIN(bt_len, buf_size - strlen(info_buf) - 1));
        }
    }
    
    return true;
}

// 保存崩溃日志到NVS
static void save_crash_log(const char *crash_info)
{
    if (!crash_info) {
        return;
    }

    ESP_LOGI(TAG, "Saving crash log: %s", crash_info);
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CRASH_LOG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    // 读取当前崩溃计数
    uint32_t count = 0;
    nvs_get_u32(nvs_handle, CRASH_COUNT_KEY, &count);
    count++;

    // 保存新的崩溃计数
    nvs_set_u32(nvs_handle, CRASH_COUNT_KEY, count);
    
    // 构建崩溃日志键名
    char key[16];
    snprintf(key, sizeof(key), CRASH_LOG_KEY_PREFIX "%u", count % 2);
    
    // 保存崩溃日志
    nvs_set_str(nvs_handle, key, crash_info);
    
    // 提交更改
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Crash log saved, count: %u", count);
}

// 处理核心转储数据
static void process_core_dump()
{
    uint32_t *buf = NULL;
    size_t len = 0;
    
    // 读取核心转储数据
    if (coredump_read(&buf, &len)) {
        ESP_LOGI(TAG, "Successfully read core dump data, size: %d bytes", len);
        
        // 保存基本信息到NVS
        char basic_info[128];
        snprintf(basic_info, sizeof(basic_info), 
                 "Core dump found, size: %d bytes", len);
        save_crash_log(basic_info);
        
        // 将整个核心转储数据转换为十六进制字符串
        char *hex_buf = malloc(len * 2 + 1);  // 每个字节需要2个字符 + 结束符
        
        if (hex_buf) {
            // 使用工具函数转换为十六进制字符串
            if (hex_to_str(hex_buf, (const uint8_t*)buf, len, false)) {
                // 确保字符串以 null 结尾
                hex_buf[len * 2] = '\0';
                
                // 打印转换后的大小
                ESP_LOGI(TAG, "Converted core dump to hex string, size: %d bytes", (int)strlen(hex_buf));
                
                // 创建描述信息
                char message[128];
                snprintf(message, sizeof(message), 
                         "Complete core dump, size: %d bytes", (int)len);
                
                // 上传完整的核心转储数据
                report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_FATAL, 
                             message, hex_buf);
            } else {
                ESP_LOGE(TAG, "Failed to convert core dump to hex string");
                report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_FATAL, 
                             basic_info, NULL);
            }
            
            // 释放内存
            free(hex_buf);
        } else {
            ESP_LOGE(TAG, "Failed to allocate memory for hex conversion");
            
            // 如果无法分配足够内存，至少上传一些基本信息
            report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_FATAL, 
                         basic_info, NULL);
        }
        
        // 清除核心转储数据
        ESP_LOGI(TAG, "Erasing core dump data");
        esp_core_dump_image_erase();
        
        // 释放内存
        free(buf);
    }
}

// 检查上次是否崩溃并读取崩溃日志
static void check_previous_crashes(void)
{
    ESP_LOGI(TAG, "Checking for previous crashes...");
    
    // 检查是否需要处理核心转储
    if (coredump_need_upload()) {
        ESP_LOGI(TAG, "System restarted after crash, processing core dump");
        process_core_dump();
    } else {
        ESP_LOGI(TAG, "Normal boot, no crash detected");
        
        // 读取之前保存的崩溃日志
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(CRASH_LOG_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
            return;
        }
        
        // 读取崩溃计数
        err = nvs_get_u32(nvs_handle, CRASH_COUNT_KEY, &crash_count);
        if (err == ESP_OK && crash_count > 0) {
            ESP_LOGI(TAG, "Found previous crash records, count: %u", crash_count);
            
            // 读取最近的崩溃日志
            char key[16];
            snprintf(key, sizeof(key), CRASH_LOG_KEY_PREFIX "%u", crash_count % 2);
            
            char crash_log[MAX_CRASH_LOG_SIZE];
            size_t required_size = sizeof(crash_log);
            
            err = nvs_get_str(nvs_handle, key, crash_log, &required_size);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "Previous crash log: %s", crash_log);
            }
        } else {
            ESP_LOGI(TAG, "No previous crash records found");
        }
        
        nvs_close(nvs_handle);
    }
}

// 手动记录崩溃日志（用于非崩溃情况下的错误记录）
void record_crash_log(const char* reason)
{
    if (!reason) {
        return;
    }
    
    char crash_log[MAX_CRASH_LOG_SIZE];
    snprintf(crash_log, sizeof(crash_log), 
             "Manual error log: %s (at %llu ms)", 
             reason, esp_timer_get_time() / 1000);
    
    save_crash_log(crash_log);
    ESP_LOGW(TAG, "Recorded manual crash log: %s", reason);
}

// 初始化崩溃处理器
void crash_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing crash handler with core dump support");
    
    // 检查上次是否崩溃
    check_previous_crashes();
    
    ESP_LOGI(TAG, "Crash handler initialized, crash count: %u", crash_count);
}
