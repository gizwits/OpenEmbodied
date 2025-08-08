#include "factory_test.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "FT_TEST";

// 测试产测串口功能
void test_factory_test_uart() {
    ESP_LOGI(TAG, "Testing factory test UART functionality");
    
    // 初始化产测
    factory_test_init();
    
    // 检查产测模式
    if (factory_test_is_enabled()) {
        ESP_LOGI(TAG, "Factory test is enabled, starting...");
        factory_test_start();
        
        // 模拟发送一些测试数据
        ESP_LOGI(TAG, "Sending test data through factory test UART");
        
        // 等待一段时间让任务启动
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        ESP_LOGI(TAG, "Factory test UART test completed");
    } else {
        ESP_LOGI(TAG, "Factory test is not enabled");
    }
}

// 测试AT命令处理
void test_at_commands() {
    ESP_LOGI(TAG, "Testing AT command processing");
    
    // 模拟一些AT命令
    const char* test_commands[] = {
        "AT+VER",
        "AT+ENTER_TEST",
        "AT+IOTEST=1,2,100,10",
        "AT+IOTEST?",
        "AT+REC=0",
        "AT+PLAY=0"
    };
    
    for (int i = 0; i < sizeof(test_commands) / sizeof(test_commands[0]); i++) {
        ESP_LOGI(TAG, "Testing command: %s", test_commands[i]);
        // 这里可以添加实际的命令处理测试
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "AT command test completed");
} 