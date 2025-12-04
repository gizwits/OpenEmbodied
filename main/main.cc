#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_event.h>
#include <cstring>
#include <cstdarg>

#include "soft_uart.h"
#include "application.h"
#include "system_info.h"
#include "watchdog.h"

#define TAG "main"

// UART1 日志输出配置
// #define UART1_LOG_NUM           UART_NUM_1
// #define UART1_LOG_TX_PIN         GPIO_NUM_2  // IO2 作为 TX
// #define UART1_LOG_RX_PIN         GPIO_NUM_1  // IO1 作为 RX
// #define UART1_LOG_BAUDRATE       115200
// #define UART1_LOG_BUF_SIZE       1024

// 全局软串口句柄
static soft_uart_port_t soft_uart_port = NULL;

// UART1 日志输出回调函数
// static int uart1_log_callback(const char* format, va_list args)
// {
//     char log_buffer[512];
//     int len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    
//     if (len > 0 && len < sizeof(log_buffer)) {
//         // 发送日志内容到 UART1
//         uart_write_bytes(UART1_LOG_NUM, log_buffer, len);
//     }
    
//     return len;
// }

// // 初始化 UART1 用于日志输出
// static esp_err_t init_uart1_log(void)
// {
//     // 配置 UART1 参数
//     uart_config_t uart_config = {
//         .baud_rate = UART1_LOG_BAUDRATE,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
//         .source_clk = UART_SCLK_DEFAULT,
//     };
    
//     // 安装 UART 驱动
//     esp_err_t ret = uart_driver_install(UART1_LOG_NUM, UART1_LOG_BUF_SIZE, 0, 0, NULL, 0);
//     if (ret != ESP_OK) {
//         return ret;
//     }
    
//     // 配置 UART 参数
//     ret = uart_param_config(UART1_LOG_NUM, &uart_config);
//     if (ret != ESP_OK) {
//         uart_driver_delete(UART1_LOG_NUM);
//         return ret;
//     }
    
//     // 设置 UART 引脚
//     ret = uart_set_pin(UART1_LOG_NUM, UART1_LOG_TX_PIN, UART1_LOG_RX_PIN, 
//                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//     if (ret != ESP_OK) {
//         uart_driver_delete(UART1_LOG_NUM);
//         return ret;
//     }

//     // 直接发送一条测试消息，方便确认 UART1 TX 是否正常工作
//     const char *init_test_msg = "UART1 init test\r\n";
//     uart_write_bytes(UART1_LOG_NUM, init_test_msg, strlen(init_test_msg));

//     // 设置日志输出回调函数，将日志重定向到 UART1
//     esp_log_set_vprintf(uart1_log_callback);
    
//     return ESP_OK;
// }

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

// ESP-IDF日志回调函数，将日志重定向到软串口
int soft_uart_log_callback(const char* format, va_list args)
{
    if (soft_uart_port == NULL) {
        return 0;
    }
    
    char log_buffer[SOFT_UART_LOG_BUFFER_SIZE];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    
    if (len > 0 && len < sizeof(log_buffer)) {
        // 发送日志内容到软串口
        soft_uart_send(soft_uart_port, (const uint8_t*)log_buffer, len);
        
        // 发送换行符
        const char* newline = "\r\n";
        soft_uart_send(soft_uart_port, (const uint8_t*)newline, strlen(newline));
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
    
    // 发送初始化成功消息到软串口
    const char* init_msg = "Soft UART initialized successfully!\r\n";
    soft_uart_send(soft_uart_port, (const uint8_t*)init_msg, strlen(init_msg));
    
    // 设置ESP-IDF日志回调，将日志重定向到软串口
    esp_log_set_vprintf(soft_uart_log_callback);
    
    ESP_LOGI(TAG, "Log output redirected to soft UART");
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
#endif
        
        soft_uart_del(soft_uart_port);
        soft_uart_port = NULL;
        ESP_LOGI(TAG, "Soft UART cleaned up");
    }
}

extern "C" void app_main(void)
{
    // 初始化 UART1 用于日志输出（IO1 RX, IO2 TX）
    // esp_err_t uart1_ret = init_uart1_log();
    // if (uart1_ret != ESP_OK) {
    //     // 如果 UART1 初始化失败，使用默认的日志输出
    //     ESP_LOGE(TAG, "UART1 log initialization failed: %s, using default log output", esp_err_to_name(uart1_ret));
    // } else {
    //     ESP_LOGI(TAG, "UART1 log initialized successfully on TX: GPIO%d, RX: GPIO%d, Baudrate: %d", 
    //              UART1_LOG_TX_PIN, UART1_LOG_RX_PIN, UART1_LOG_BAUDRATE);
    // }

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
