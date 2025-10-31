#ifndef ERROR_MONITOR_H
#define ERROR_MONITOR_H

#include "esp_err.h"

// 错误类型枚举
typedef enum {
    ERROR_TYPE_MEMORY = 0,
    ERROR_TYPE_NETWORK,
    ERROR_TYPE_BATTERY,
    ERROR_TYPE_AUDIO,
    ERROR_TYPE_SYSTEM
} error_type_t;

// 错误级别
typedef enum {
    ERROR_LEVEL_INFO = 0,
    ERROR_LEVEL_WARNING,
    ERROR_LEVEL_ERROR,
    ERROR_LEVEL_FATAL
} error_level_t;

// 初始化错误监控
esp_err_t error_monitor_init(void);

// 报告错误
esp_err_t report_error(error_type_t type, error_level_t level, 
                       const char* message, const char* stack);

// 上传堆栈作为附件
esp_err_t upload_stack_as_attachment(const char* event_id, const char* stack);

#endif 