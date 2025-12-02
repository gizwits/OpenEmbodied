#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <cstring>
#include <cstdarg>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "soft_uart.h"
#include "application.h"
#include "system_info.h"
#include "watchdog.h"

#define TAG "main"

// 全局软串口句柄
static soft_uart_port_t soft_uart_port = NULL;
static QueueHandle_t soft_uart_log_queue = NULL;
static TaskHandle_t soft_uart_log_task_handle = NULL;

#if SOFT_UART_LOG_ENABLED

// 软串口日志输出函数
void soft_uart_log_output(const char* tag, esp_log_level_t level, const char* format, ...)
{
    if (soft_uart_port == NULL) {
        return;
    }
    
    // 构建日志前缀
    const char* level_str[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"};
    char prefix[SOFT_UART_LOG_PREFIX_SIZE];
    snprintf(prefix, sizeof(prefix), "[%s][%s] ", level_str[level], tag);
    
    // 发送前缀
    soft_uart_send(soft_uart_port, (const uint8_t*)prefix, strlen(prefix));
    
    // 处理可变参数
    va_list args;
    va_start(args, format);
    char log_buffer[SOFT_UART_LOG_BUFFER_SIZE];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    
    if (len > 0 && len < sizeof(log_buffer)) {
        // 发送日志内容
        soft_uart_send(soft_uart_port, (const uint8_t*)log_buffer, len);
        
        // 发送换行符
        const char* newline = "\r\n";
        soft_uart_send(soft_uart_port, (const uint8_t*)newline, strlen(newline));
    }
}

// 软串口日志发送任务
static void soft_uart_log_task(void* arg) {
    char log_buffer[SOFT_UART_LOG_BUFFER_SIZE + 2];  // +2 for \r\n
    UBaseType_t queue_size = 0;
    
    while (1) {
        if (xQueueReceive(soft_uart_log_queue, log_buffer, portMAX_DELAY) == pdTRUE) {
            if (soft_uart_port != NULL) {
                // 查找字符串长度（以\0结尾）
                size_t len = strlen(log_buffer);
                if (len > 0) {
                    // 发送日志内容到软串口
                    soft_uart_send(soft_uart_port, (const uint8_t*)log_buffer, len);
                }
            }
            
            // 检查队列剩余空间，如果队列快满了，给其他任务一些时间
            queue_size = uxQueueMessagesWaiting(soft_uart_log_queue);
            if (queue_size > 80) {  // 队列使用超过80%，稍作延迟
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
}

// ESP-IDF日志回调函数，将日志重定向到软串口（异步发送，避免阻塞）
int soft_uart_log_callback(const char* format, va_list args)
{
    if (soft_uart_port == NULL || soft_uart_log_queue == NULL) {
        return 0;
    }
    
    char log_buffer[SOFT_UART_LOG_BUFFER_SIZE];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    
    if (len > 0 && len < sizeof(log_buffer)) {
        // 添加换行符
        if (len < sizeof(log_buffer) - 2) {
            log_buffer[len] = '\r';
            log_buffer[len + 1] = '\n';
            log_buffer[len + 2] = '\0';
            len += 2;
        }
        
        // 异步发送到队列（非阻塞）
        if (xQueueSend(soft_uart_log_queue, log_buffer, 0) != pdTRUE) {
            // 队列满时丢弃日志，避免阻塞
            // 注意：这里不能使用ESP_LOG，否则会递归调用导致死锁
            // 可以通过硬件串口输出警告（如果可用）
        }
    }
    
    return len;
}

#endif // SOFT_UART_LOG_ENABLED

// 初始化软串口
esp_err_t init_soft_uart(void)
{
#if SOFT_UART_LOG_ENABLED
    soft_uart_config_t config = {
        .tx_pin = SOFT_UART_TX_PIN,
        .rx_pin = SOFT_UART_RX_PIN,
        .baudrate = SOFT_UART_BAUDRATE
    };
    
    esp_err_t ret = soft_uart_new(&config, &soft_uart_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize soft UART: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Soft UART initialized successfully on TX: %d, RX: %d, Baudrate: %d", 
             SOFT_UART_TX_PIN, SOFT_UART_RX_PIN, SOFT_UART_BAUDRATE);
    
    // 创建日志队列（最多缓存100条日志，增加队列大小避免丢失）
    soft_uart_log_queue = xQueueCreate(100, SOFT_UART_LOG_BUFFER_SIZE + 2);
    if (soft_uart_log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create soft UART log queue");
        soft_uart_del(soft_uart_port);
        soft_uart_port = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // 创建日志发送任务（优先级2，确保能及时发送日志）
    xTaskCreate(soft_uart_log_task, "soft_uart_log", 3072, NULL, 2, &soft_uart_log_task_handle);
    if (soft_uart_log_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create soft UART log task");
        vQueueDelete(soft_uart_log_queue);
        soft_uart_log_queue = NULL;
        soft_uart_del(soft_uart_port);
        soft_uart_port = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // 发送初始化成功消息到软串口
    const char* init_msg = "Soft UART initialized successfully!\r\n";
    soft_uart_send(soft_uart_port, (const uint8_t*)init_msg, strlen(init_msg));
    
    // 设置ESP-IDF日志回调，将日志重定向到软串口
    esp_log_set_vprintf(soft_uart_log_callback);
    
    ESP_LOGI(TAG, "Log output redirected to soft UART (async mode)");
#else
    ESP_LOGI(TAG, "Soft UART logging is disabled in configuration");
    return ESP_OK;
#endif

#if SOFT_UART_DEBUG_ENABLED && SOFT_UART_LOG_ENABLED
    // 发送调试信息到软串口
    char debug_msg[128];
    snprintf(debug_msg, sizeof(debug_msg), "Debug: TX_PIN=%d, RX_PIN=%d, Baudrate=%d\r\n",
             SOFT_UART_TX_PIN, SOFT_UART_RX_PIN, SOFT_UART_BAUDRATE);
    soft_uart_send(soft_uart_port, (const uint8_t*)debug_msg, strlen(debug_msg));
#endif
    
    return ESP_OK;
}

// 清理软串口
void cleanup_soft_uart(void)
{
    if (soft_uart_port != NULL) {
#if SOFT_UART_LOG_ENABLED
        // 恢复默认的日志输出
        esp_log_set_vprintf(vprintf);
        
        // 删除日志任务
        if (soft_uart_log_task_handle != NULL) {
            vTaskDelete(soft_uart_log_task_handle);
            soft_uart_log_task_handle = NULL;
        }
        
        // 删除日志队列
        if (soft_uart_log_queue != NULL) {
            vQueueDelete(soft_uart_log_queue);
            soft_uart_log_queue = NULL;
        }
#endif
        
        soft_uart_del(soft_uart_port);
        soft_uart_port = NULL;
        ESP_LOGI(TAG, "Soft UART cleaned up");
    }
}

extern "C" void app_main(void)
{

#if SOFT_UART_LOG_ENABLED
    // 初始化软串口
    esp_err_t uart_ret = init_soft_uart();
    if (uart_ret != ESP_OK) {
        ESP_LOGE(TAG, "Soft UART initialization failed, continuing without it");
    }
    
    // 发送启动消息到软串口
    if (soft_uart_port != NULL) {
        const char* start_msg = "Xiaozhi ESP32 starting up...\r\n";
        soft_uart_send(soft_uart_port, (const uint8_t*)start_msg, strlen(start_msg));
    }
    
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#else
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#endif

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    
    esp_log_level_set("adc_hal", ESP_LOG_INFO);
    esp_log_level_set("LedSignal", ESP_LOG_INFO);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
    auto& watchdog = Watchdog::GetInstance();
    watchdog.SubscribeTask(xTaskGetCurrentTaskHandle());
    
#if SOFT_UART_LOG_ENABLED
    // 发送应用启动成功消息到软串口
    if (soft_uart_port != NULL) {
        const char* app_msg = "Application started successfully\r\n";
        soft_uart_send(soft_uart_port, (const uint8_t*)app_msg, strlen(app_msg));
    }
#endif
    
    app.MainEventLoop();
    
    // 清理软串口（通常不会执行到这里）
    cleanup_soft_uart();
}
