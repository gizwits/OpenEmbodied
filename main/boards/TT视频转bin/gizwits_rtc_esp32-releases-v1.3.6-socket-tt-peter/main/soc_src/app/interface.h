/*adapter H file of ESPsdk  nonOS2.x -> RTOS4.3*/
#ifndef _INTERFACE_H_
#define _INTERFACE_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "os.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gattypes.h"

#define STATION_IF      0x00
#define SOFTAP_IF       0x01

#define LOCAL static
#define ICACHE_FLASH_ATTR 
// #define os_signal_t ETSSignal
// #define os_param_t  ETSParam
typedef struct {
    uint32_t sig;
    uint32_t par;
}os_event_t;

// #define os_event_t QueueHandle_t
#define os_task_t TaskHandle_t

typedef struct ETSEventTag ETSEvent;
typedef void (*TaskFunction_t)( void * );
typedef void (*ETSTask)(ETSEvent *e);

typedef esp_timer_handle_t os_timer_t;
typedef void os_timer_func_t(void *timer_arg);
typedef struct userTaskData
{
    os_task_t task;
    os_event_t *queue;
    uint8_t qlen;
}userTaskData_t;

enum {
    USER_TASK_PRIO_0 = 0,
    USER_TASK_PRIO_1,
    USER_TASK_PRIO_2,
    USER_TASK_PRIO_MAX
};


bool system_os_task();
bool system_os_post(uint8_t prio, uint32_t sig, uint32_t par);
void user_handle(void *arg);
void system_restart(void);
// void gagentProcessRun(os_event_t * events);
bool timer_setfn(esp_timer_handle_t *timer, esp_timer_cb_t callback, void* cb_arg);
void timer_arm(esp_timer_handle_t *timer, uint64_t unit, uint64_t period);
void timer_disarm(esp_timer_handle_t *timer);

#endif