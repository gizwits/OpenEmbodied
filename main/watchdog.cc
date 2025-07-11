#include "watchdog.h"
#include <esp_log.h>

#define TAG "Watchdog"

// 示例任务函数，展示如何使用看门狗
void WatchdogTask(void* arg) {
    // 获取当前任务句柄
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    
    // 初始化看门狗
    auto& watchdog = Watchdog::GetInstance();
    // watchdog.Initialize(10, true);  // 10秒超时，超时后触发系统复位
    
    // 订阅当前任务到看门狗
    watchdog.SubscribeTask(task_handle);
    
    ESP_LOGI(TAG, "Watchdog task started");
    
    while (1) {
        // 定期喂狗
        watchdog.Reset();
        vTaskDelay(pdMS_TO_TICKS(1000));  // 延时1秒
    }
    
    // 如果任务结束，取消订阅
    watchdog.UnsubscribeTask(task_handle);
    vTaskDelete(NULL);
} 