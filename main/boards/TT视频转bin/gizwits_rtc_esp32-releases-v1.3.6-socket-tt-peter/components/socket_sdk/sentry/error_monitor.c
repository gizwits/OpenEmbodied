#include <string.h>
#include <time.h>
#include "error_monitor.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include "cJSON.h"
#include "esp_mac.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_err_t _report_error(error_type_t type, error_level_t level, 
                              const char* message, const char* stack);


#define ERROR_MONITOR_URL "http://appmonitor.gizwits.com/api/%s/store/?sentry_key=%s&sentry_version=7"

#define TAG "ERROR_MONITOR"
#if CONFIG_LOG_DEFAULT_LEVEL_ERROR
#define SENTRY_KEY "8d6a7159c3af4341868c523b82b64317"
#define PROJECT_ID "82"
#else
#define SENTRY_KEY "dfcfb75102714e289103393705eaec61"
#define PROJECT_ID "85"
#endif

char device_id[13] = {0};  // MAC地址作为设备ID
static void print_curl_command(const char* url, const char* content_type, const char* post_data, size_t post_len);

// 定义上传任务的栈大小
#define UPLOAD_TASK_STACK_SIZE 8192

// 上传任务的参数结构
typedef struct {
    char event_id[33];
    char *stack;
} upload_task_params_t;

// 上传任务函数
static void upload_stack_task(void *pvParameters) {
    upload_task_params_t *params = (upload_task_params_t *)pvParameters;
    
    if (params && params->stack) {
        // 打印堆栈长度
        ESP_LOGE(TAG, "Stack upload task: stack length = %d bytes", (int)strlen(params->stack));
        
        // 上传堆栈信息
        ESP_LOGE(TAG, "Starting stack upload task for event %s", params->event_id);
        upload_stack_as_attachment(params->event_id, params->stack);
        
        // 释放资源
        free(params->stack);
        free(params);
    } else {
        ESP_LOGE(TAG, "Invalid parameters for stack upload task");
    }
    
    // 删除任务
    vTaskDelete(NULL);
}

// 生成事件ID (使用CRC32)
static uint32_t generate_event_id(void) {
    uint64_t timestamp = esp_timer_get_time();
    return esp_crc32_le(0, (const uint8_t*)&timestamp, sizeof(timestamp));
}

// 获取错误类型字符串
static const char* get_error_type_str(error_type_t type) {
    switch(type) {
        case ERROR_TYPE_MEMORY: return "MemoryError";
        case ERROR_TYPE_NETWORK: return "NetworkError";
        case ERROR_TYPE_BATTERY: return "BatteryError";
        case ERROR_TYPE_AUDIO: return "AudioError";
        case ERROR_TYPE_SYSTEM: return "SystemError";
        default: return "UnknownError";
    }
}

// 获取错误级别字符串
static const char* get_error_level_str(error_level_t level) {
    switch(level) {
        case ERROR_LEVEL_INFO: return "info";
        case ERROR_LEVEL_WARNING: return "warning";
        case ERROR_LEVEL_ERROR: return "error";
        case ERROR_LEVEL_FATAL: return "fatal";
        default: return "error";
    }
}

esp_err_t error_monitor_init(void) {
    char mac_str[13];
    get_mac_str(mac_str, true);
    snprintf(device_id, sizeof(device_id), "%s", mac_str);
    return ESP_OK;
}

// 错误报告参数结构
typedef struct {
    error_type_t type;
    error_level_t level;
    char *message;
    char *stack;
} error_report_params_t;

// 线程化错误报告函数
static void report_error_task(void *pvParameters) {
    error_report_params_t *params = (error_report_params_t *)pvParameters;
    
    if (params) {
        // 调用实际的错误报告函数
        _report_error(params->type, params->level, params->message, params->stack);
        
        // 释放参数内存
        if (params->message) free(params->message);
        if (params->stack) free(params->stack);
        free(params);
    } else {
        ESP_LOGE(TAG, "Invalid parameters for error report task");
    }
    
    // 删除任务
    vTaskDelete(NULL);
}

// 公共错误报告接口
esp_err_t report_error(error_type_t type, error_level_t level, 
                       const char* message, const char* stack) {
    // 创建参数结构
    error_report_params_t *params = malloc(sizeof(error_report_params_t));
    if (!params) {
        ESP_LOGE(TAG, "Failed to allocate memory for error report parameters");
        return ESP_ERR_NO_MEM;
    }
    
    // 设置参数
    params->type = type;
    params->level = level;
    
    // 复制消息字符串
    if (message) {
        params->message = strdup(message);
        if (!params->message) {
            ESP_LOGE(TAG, "Failed to allocate memory for error message");
            free(params);
            return ESP_ERR_NO_MEM;
        }
    } else {
        params->message = NULL;
    }
    
    // 复制堆栈字符串
    if (stack) {
        params->stack = strdup(stack);
        if (!params->stack) {
            ESP_LOGE(TAG, "Failed to allocate memory for stack trace");
            if (params->message) free(params->message);
            free(params);
            return ESP_ERR_NO_MEM;
        }
    } else {
        params->stack = NULL;
    }
    
    // 创建任务
    BaseType_t result = xTaskCreate(
        report_error_task,    // 任务函数
        "report_error",       // 任务名称
        8192,                 // 栈大小 (增加到8192以处理大型JSON)
        params,               // 任务参数
        5,                    // 优先级 (中等)
        NULL                  // 任务句柄
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create error report task");
        if (params->message) free(params->message);
        if (params->stack) free(params->stack);
        free(params);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Error report task created successfully");
    return ESP_OK;
}

// 实际的错误报告实现
static esp_err_t _report_error(error_type_t type, error_level_t level, 
                              const char* message, const char* stack) {
    cJSON *root = cJSON_CreateObject();
    
    if (!root) {
        ESP_LOGE(TAG, "Error reporting failed: %s", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    
    // 创建异常信息
    cJSON *exception = cJSON_CreateObject();
    cJSON *values = cJSON_CreateArray();
    cJSON *value = cJSON_CreateObject();
    
    cJSON_AddStringToObject(value, "type", get_error_type_str(type));
    cJSON_AddStringToObject(value, "value", message ? message : "Unknown error");
    
    // 添加空的 stacktrace 对象
    cJSON *stacktrace = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    cJSON_AddItemToObject(stacktrace, "frames", frames);
    cJSON_AddItemToObject(value, "stacktrace", stacktrace);
    
    // 添加 mechanism
    cJSON *mechanism = cJSON_CreateObject();
    cJSON_AddStringToObject(mechanism, "type", "generic");
    cJSON_AddBoolToObject(mechanism, "handled", true);
    cJSON_AddItemToObject(value, "mechanism", mechanism);
    
    cJSON_AddItemToArray(values, value);
    cJSON_AddItemToObject(exception, "values", values);
    cJSON_AddItemToObject(root, "exception", exception);
    
    // 添加基本信息
    char event_id[33] = {0};
    uint64_t timestamp = esp_timer_get_time() / 1000;
    // 使用时间戳和MAC地址生成32位随机字符串
    uint8_t random_data[16];
    esp_fill_random(random_data, sizeof(random_data));
    snprintf(event_id, sizeof(event_id), 
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             random_data[0], random_data[1], random_data[2], random_data[3],
             random_data[4], random_data[5], random_data[6], random_data[7],
             random_data[8], random_data[9], random_data[10], random_data[11],
             random_data[12], random_data[13], random_data[14], random_data[15]);
    
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "level", get_error_level_str(level));
    cJSON_AddStringToObject(root, "platform", "c");
    cJSON_AddNumberToObject(root, "timestamp", (double)esp_timer_get_time() / 1000000.0);
    
    // 添加空的 sdk 对象
    cJSON *sdk = cJSON_CreateObject();
    cJSON_AddStringToObject(sdk, "name", "esp32s3");
    cJSON_AddStringToObject(sdk, "version", sdk_get_software_version());
    cJSON_AddItemToObject(root, "sdk", sdk);
    
    // 添加 environment
    cJSON_AddStringToObject(root, "environment", "production");
    
    // 添加 contexts
    cJSON *contexts = cJSON_CreateObject();
    cJSON *device = cJSON_CreateObject();
    cJSON *os = cJSON_CreateObject();
    cJSON *extra = cJSON_CreateObject();
    
    cJSON_AddStringToObject(os, "name", "esp32s3");
    cJSON_AddStringToObject(os, "version", sdk_get_software_version());
    
    cJSON_AddStringToObject(extra, "mac", device_id);
    
    cJSON_AddStringToObject(device, "mac", device_id);

    cJSON_AddItemToObject(contexts, "device", device);
    cJSON_AddItemToObject(contexts, "os", os);
    cJSON_AddItemToObject(contexts, "extra", extra);
    cJSON_AddItemToObject(root, "contexts", contexts);
    
    // 添加 extra
    cJSON *extra_root = cJSON_CreateObject();
    if (message) {
        cJSON_AddStringToObject(extra_root, "message", message);
        
        // 尝试从错误消息中提取错误码
        if (strstr(message, "0x") != NULL) {
            cJSON_AddStringToObject(extra_root, "error_code", message);
        }
    }
    
    cJSON_AddItemToObject(root, "extra", extra_root);
    
    // 转换为字符串
    char *post_data = cJSON_PrintUnformatted(root);
    
    // 保存事件ID用于后续上传堆栈
    char saved_event_id[33];
    strncpy(saved_event_id, event_id, sizeof(saved_event_id) - 1);
    saved_event_id[sizeof(saved_event_id) - 1] = '\0';
    
    cJSON_Delete(root);
    
    if (!post_data) {
        ESP_LOGE(TAG, "Error reporting failed: %s", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    
    // 配置HTTP客户端
    char url[256];
    snprintf(url, sizeof(url), ERROR_MONITOR_URL, PROJECT_ID, SENTRY_KEY);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(post_data);
        return ESP_FAIL;
    }
    
    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Error reported successfully, status: %d", status);
        
        // 读取响应内容
        char buffer[512] = {0};
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (read_len > 0) {
            buffer[read_len] = 0;
            ESP_LOGI(TAG, "Response: %s", buffer);
        }
        
        // 检查堆栈信息是否有效
        if (stack && strlen(stack) > 0) {
            ESP_LOGI(TAG, "Stack trace provided, length: %d bytes", (int)strlen(stack));
            
            // 如果堆栈信息太大，不要在日志中打印
            if (strlen(stack) > 100) {
                ESP_LOGI(TAG, "Stack trace too large to print in log");
                ESP_LOGI(TAG, "Stack trace preview: %.100s...", stack);
            } else {
                ESP_LOGI(TAG, "Stack trace: %s", stack);
            }
            
            // 创建一个单独的任务来上传堆栈信息
            upload_task_params_t *params = malloc(sizeof(upload_task_params_t));
            if (params) {
                strncpy(params->event_id, saved_event_id, sizeof(params->event_id) - 1);
                params->event_id[sizeof(params->event_id) - 1] = '\0';
                
                // 复制堆栈信息
                params->stack = strdup(stack);
                if (params->stack) {
                    ESP_LOGI(TAG, "Created stack copy of %d bytes", (int)strlen(params->stack));
                    xTaskCreate(upload_stack_task, "stack_upload", UPLOAD_TASK_STACK_SIZE, 
                               params, tskIDLE_PRIORITY + 1, NULL);
                } else {
                    ESP_LOGE(TAG, "Failed to allocate memory for stack copy");
                    free(params);
                }
            }
        } else {
            ESP_LOGI(TAG, "No stack trace provided");
        }
    } else {
        ESP_LOGE(TAG, "Error reporting failed: %s", esp_err_to_name(err));
    }
    
    // 清理资源
    free(post_data);
    esp_http_client_cleanup(client);
    
    return (err == ESP_OK && status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

// 修改upload_stack_as_attachment函数，优化内存使用
esp_err_t upload_stack_as_attachment(const char* event_id, const char* stack) {
    if (stack == NULL || event_id == NULL) {
        ESP_LOGE(TAG, "Stack or event_id is NULL, skipping attachment upload");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGE(TAG, "Uploading stack trace as attachment for event %s", event_id);
    
    // 移除过于严格的堆栈指针验证
    // 只检查字符串是否有效
    size_t stack_len = strnlen(stack, 65536 * 2); // 增加到 128KB
    if (stack_len == 0) {
        ESP_LOGE(TAG, "Stack trace is empty");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (stack_len >= 65536 * 2) {
        ESP_LOGE(TAG, "Stack trace too long, truncating to 128KB");
        stack_len = 65536  * 2 - 1;
    }
    
    // 构建上传URL
    char upload_url[256];
    snprintf(upload_url, sizeof(upload_url), 
             "http://appmonitor.gizwits.com/api/%s/events/%s/attachments/?sentry_key=%s&sentry_version=7", 
             PROJECT_ID, event_id, SENTRY_KEY);
    
    // 使用更简单的boundary，避免特殊字符
    const char *boundary = "boundary123456789";
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    
    // 创建堆栈数据的安全副本
    char *stack_copy = malloc(stack_len + 1);
    if (!stack_copy) {
        ESP_LOGE(TAG, "Failed to allocate memory for stack copy");
        return ESP_ERR_NO_MEM;
    }
    
    // 安全复制堆栈数据
    memcpy(stack_copy, stack, stack_len);
    stack_copy[stack_len] = '\0';
    
    // 打印堆栈长度信息
    ESP_LOGI(TAG, "Stack trace length: %d bytes", (int)stack_len);
    
    // 打印堆栈内容的前100个字符（用于调试）
    if (stack_len > 0) {
        char preview[101] = {0};
        size_t preview_len = stack_len > 100 ? 100 : stack_len;
        memcpy(preview, stack, preview_len);
        preview[preview_len] = '\0';
        ESP_LOGI(TAG, "Stack trace preview: %s%s", 
                 preview, stack_len > 100 ? "..." : "");
    }
    
    // 使用更简单的表单格式，确保格式完全符合标准
    // 注意：每个部分之间必须有正确的CRLF分隔符
    const char *form_data_fmt = 
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n"
        "\r\n"
        "stack_trace.txt\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"attachment_type\"\r\n"
        "\r\n"
        "event.attachment\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"attachment\"; filename=\"stack_trace.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "%s\r\n"
        "--%s--\r\n";
    
    // 计算表单数据大小
    size_t form_data_size = snprintf(NULL, 0, form_data_fmt, 
                                    boundary, boundary, boundary, stack_copy, boundary);
    
    // 分配内存
    char *form_data = malloc(form_data_size + 1);
    if (!form_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for form data");
        free(stack_copy);
        return ESP_ERR_NO_MEM;
    }
    
    // 填充表单数据
    int actual_size = snprintf(form_data, form_data_size + 1, form_data_fmt, 
                              boundary, boundary, boundary, stack_copy, boundary);
    
    // 不再需要堆栈副本
    free(stack_copy);
    
    // 确保实际大小与预期大小一致
    if (actual_size != form_data_size) {
        ESP_LOGE(TAG, "Warning: Actual form data size (%d) differs from calculated size (%d)", 
                 actual_size, form_data_size);
        form_data_size = actual_size;
    }
    
    // 配置HTTP客户端
    esp_http_client_config_t config = {
        .url = upload_url,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(form_data);
        return ESP_FAIL;
    }
    
    // 设置请求头
    esp_http_client_set_header(client, "Content-Type", content_type);

    // 将 Content-Length 转换为字符串
    char content_length_str[16];
    snprintf(content_length_str, sizeof(content_length_str), "%d", (int)form_data_size);
    esp_http_client_set_header(client, "Content-Length", content_length_str);

    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "User-Agent", "ESP32/1.0");
    
    // 使用一次性发送方式，而不是流式发送
    esp_http_client_set_post_field(client, form_data, form_data_size);
    
    // 执行请求
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(form_data);
        esp_http_client_cleanup(client);
        return err;
    }
    
    // 获取响应
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGE(TAG, "Attachment upload status code: %d", status_code);
    
    // 读取响应内容
    char buffer[1024] = {0};
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    if (read_len > 0) {
        buffer[read_len] = 0;
        ESP_LOGE(TAG, "Attachment upload response (%d bytes): %s", read_len, buffer);
    } else {
        ESP_LOGE(TAG, "No response body or error reading response");
    }
    
    // 清理资源
    free(form_data);
    esp_http_client_cleanup(client);
    
    return (status_code >= 200 && status_code < 300) ? ESP_OK : ESP_FAIL;
}

// 添加一个函数来打印等效的cURL命令
static void print_curl_command(const char* url, const char* content_type, const char* post_data, size_t post_len) {
    // 创建临时文件来存储POST数据
    const char* temp_file = "/tmp/curl_data.txt";
    FILE* f = fopen(temp_file, "wb");
    if (f) {
        fwrite(post_data, 1, post_len, f);
        fclose(f);
        
        // 打印cURL命令
        ESP_LOGE(TAG, "Equivalent cURL command:");
        ESP_LOGE(TAG, "curl -v -X POST \\\n"
                      "  -H \"Content-Type: %s\" \\\n"
                      "  -H \"Connection: keep-alive\" \\\n"
                      "  -H \"User-Agent: ESP32/1.0\" \\\n"
                      "  --data-binary @%s \\\n"
                      "  \"%s\"", 
                 content_type, temp_file, url);
        
        // 打印数据内容（限制长度以避免日志过长）
        ESP_LOGE(TAG, "POST data (first 500 bytes):");
        int print_len = (post_len > 500) ? 500 : post_len;
        char preview[501] = {0};
        memcpy(preview, post_data, print_len);
        ESP_LOGE(TAG, "%s%s", preview, (post_len > 500) ? "..." : "");
    } else {
        ESP_LOGE(TAG, "Failed to create temp file for cURL command");
        
        // 如果无法创建文件，直接打印命令
        ESP_LOGE(TAG, "Equivalent cURL command (without data):");
        ESP_LOGE(TAG, "curl -v -X POST \\\n"
                      "  -H \"Content-Type: %s\" \\\n"
                      "  -H \"Connection: keep-alive\" \\\n"
                      "  -H \"User-Agent: ESP32/1.0\" \\\n"
                      "  --data-binary \"[DATA_TOO_LARGE_TO_PRINT]\" \\\n"
                      "  \"%s\"", 
                 content_type, url);
    }
} 