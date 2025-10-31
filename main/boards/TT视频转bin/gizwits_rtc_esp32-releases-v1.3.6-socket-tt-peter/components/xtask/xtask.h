#ifndef _XTASK_H_
#define _XTASK_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建任务并检查结果
 * @param task_func 任务函数
 * @param name 任务名称
 * @param stack_size 堆栈大小
 * @param arg 任务参数
 * @param priority 任务优先级
 * @param task_handle 任务句柄指针
 * @return true成功，false失败
 */
bool xTaskCreateExt(TaskFunction_t task_func, 
                       const char *name, 
                       uint32_t stack_size, 
                       void *arg, 
                       UBaseType_t priority, 
                       TaskHandle_t *task_handle);

/**
 * @brief 创建任务并绑定到指定核心
 * @param task_func 任务函数
 * @param name 任务名称
 * @param stack_size 堆栈大小
 * @param arg 任务参数
 * @param priority 任务优先级
 * @param task_handle 任务句柄指针
 * @param core_id 核心ID (0或1)
 * @return true成功，false失败
 */
bool xTaskCreateExtPinnedToCore(TaskFunction_t task_func,
                       const char *name,
                       uint32_t stack_size,
                       void *arg,
                       UBaseType_t priority,
                       TaskHandle_t *task_handle,
                       BaseType_t core_id);

/**
 * @brief 删除任务并检查结果
 * @param task_handle 任务句柄
 * @return true成功，false失败
 */
bool xTaskDeleteExt(TaskHandle_t task_handle);

#ifdef __cplusplus
}
#endif

#endif /* _XTASK_H_ */
