#include "esp_heap_caps.h"
#include "xtask.h"

static const char *TAG = "XTASK";

/**
 * @brief 创建任务并检查结果
 * @param task_func 任务函数
 * @param name 任务名称
 * @param stack_size 堆栈大小
 * @param arg 任务参数
 * @param priority 任务优先级
 * @param task_handle 任务句柄指针（可为NULL）
 * @return true成功，false失败
 */
bool xTaskCreateExt(TaskFunction_t task_func,
                       const char *name,
                       uint32_t stack_size,
                       void *arg,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle)
{
    TaskHandle_t local_handle;
    TaskHandle_t *handle_ptr = task_handle ? task_handle : &local_handle;

    ESP_LOGI(TAG, "xTaskCreateExt: %s, stack: %d, priority: %d", name, stack_size, priority);
    // 在SPIRAM中分配任务栈
    StackType_t *task_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    if (!task_stack) {
        ESP_LOGE(TAG, "Failed to allocate task stack in SPIRAM");
        return false;
    }

    // 创建任务控制块
    StaticTask_t *task_tcb = (StaticTask_t *)malloc(sizeof(StaticTask_t));
    if (!task_tcb) {
        ESP_LOGE(TAG, "Failed to allocate task TCB in SPIRAM");
        heap_caps_free(task_stack);
        return false;
    }

    // 使用静态方式创建任务
    *handle_ptr = xTaskCreateStatic(
        task_func,
        name,
        stack_size,
        arg,
        priority,
        task_stack,
        task_tcb);

    if (*handle_ptr == NULL) {
        ESP_LOGE(TAG, "Failed to create task");
        heap_caps_free(task_stack);
        heap_caps_free(task_tcb);
        return false;
    }

    ESP_LOGI(TAG, "xTaskCreateExt: %s done", name);
    return true;
}


/**
 * @brief 创建任务并绑定到指定核心
 * @param task_func 任务函数
 * @param name 任务名称
 * @param stack_size 堆栈大小
 * @param arg 任务参数
 * @param priority 任务优先级
 * @param task_handle 任务句柄指针（可为NULL）
 * @param core_id 核心ID (0或1)
 * @return true成功，false失败
 */
bool xTaskCreateExtPinnedToCore(TaskFunction_t task_func,
                       const char *name,
                       uint32_t stack_size,
                       void *arg,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle,
                       BaseType_t core_id)
{
    TaskHandle_t local_handle;
    TaskHandle_t *handle_ptr = task_handle ? task_handle : &local_handle;

    ESP_LOGI(TAG, "xTaskCreateExtPinnedToCore: %s, stack: %d, priority: %d", name, stack_size, priority);
    // 在SPIRAM中分配任务栈
    StackType_t *task_stack = (StackType_t *)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    if (!task_stack) {
        ESP_LOGE(TAG, "Failed to allocate task stack in SPIRAM");
        return false;
    }

    // 创建任务控制块
    StaticTask_t *task_tcb = (StaticTask_t *)malloc(sizeof(StaticTask_t));
    if (!task_tcb) {
        ESP_LOGE(TAG, "Failed to allocate task TCB in SPIRAM");
        heap_caps_free(task_stack);
        return false;
    }

    // 使用静态方式创建任务并绑定到指定核心
    *handle_ptr = xTaskCreateStaticPinnedToCore(
        task_func,
        name,
        stack_size,
        arg,
        priority,
        task_stack,
        task_tcb,
        core_id);

    if (*handle_ptr == NULL) {
        ESP_LOGE(TAG, "Failed to create task");
        heap_caps_free(task_stack);
        heap_caps_free(task_tcb);
        return false;
    }

    ESP_LOGI(TAG, "xTaskCreateExtPinnedToCore on core %d done", core_id);
    return true;
}

/**
 * @brief 删除任务并检查结果
 * @param task_handle 任务句柄
 * @return true成功，false失败
 */
bool xTaskDeleteExt(TaskHandle_t task_handle)
{
    // 获取任务栈和控制块指针
    StackType_t *task_stack = (StackType_t *)pxTaskGetStackStart(task_handle);
    StaticTask_t *task_tcb = (StaticTask_t *)pvTaskGetThreadLocalStoragePointer(task_handle, 0);

    // 删除任务
    vTaskDelete(task_handle);

    // 释放SPIRAM中的内存
    if (task_stack) {
        heap_caps_free(task_stack);
    }
    if (task_tcb) {
        free(task_tcb);
    }

    ESP_LOGI(TAG, "Task deleted successfully");
    return true;
}
