/*adapter C file of ESPsdk  nonOS2.x -> RTOS4.3*/

#include "interface.h"
#include "gagent_soc.h"
#include "mqtt.h"

// static StackType_t user_task_stack[4 * 1024];
// static StaticTask_t user_task_buffer;

void gagentProcessRun(os_event_t *events)
{
    return;
}

static xQueueHandle user_evt_queue = NULL;

bool system_os_task()
{

    user_evt_queue = xQueueCreate(30, sizeof(os_event_t));
    if (user_evt_queue == NULL)
    {
        printf("user_evt_queue create fail!!!");
        return ESP_FAIL;
    }

    TaskHandle_t user_task_handle = xTaskCreate(
        user_handle, 
        "USER_TASK_PRIO_2", 
        4 * 1024, 
        NULL, 
        5, 
        NULL
    );
    if (user_task_handle == NULL)
    {
        printf("user_handle xTaskCreateStatic create fail!!!");
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool system_os_post(uint8_t prio, uint32_t sig, uint32_t par)
{
    os_event_t evt;

    if (prio != USER_TASK_PRIO_2)
    {
        return ESP_FAIL;
    }

    evt.sig = sig;
    evt.par = par;

    if(user_evt_queue == NULL)
    {
        return ESP_FAIL;
    }

    int32_t ret = xQueueSendToBack((xQueueHandle)user_evt_queue, (void*)&evt, (TickType_t)10);

    if (pdPASS != ret)
    {
        printf("system_os_post fail!!!");
        return ESP_FAIL;
    }
    else {
        // printf( "system_os_post success!!! sig: %d, par: %d\n", sig, par);
    }
    return ESP_OK;
}

extern void ICACHE_FLASH_ATTR gizwitsTask(os_event_t *events);
void user_handle(void *arg)
{
    while (1)
    {
        os_event_t evt;
        if (xQueueReceive(user_evt_queue, &evt, portMAX_DELAY) == pdPASS)
        {
            gizwitsTask(&evt);
        }
    }
}

void system_restart(void)
{
    esp_restart();
}

bool timer_setfn(esp_timer_handle_t *timer, esp_timer_cb_t callback, void *cb_arg)
{
    esp_timer_create_args_t timer_args = {
        .callback = callback,
        .arg = cb_arg};

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, timer));
    return ESP_OK;
}

void timer_arm(esp_timer_handle_t *timer, uint64_t unit, uint64_t period)
{
    ESP_ERROR_CHECK(esp_timer_start_periodic(*timer, 1000 * unit * period));
}

void timer_disarm(esp_timer_handle_t *timer)
{
    if (*timer != NULL)
    {
        ESP_ERROR_CHECK(esp_timer_stop(*timer));
    }
}


/*
开始处理MQTT 组包业务
*/

int32 gagentUploadData(uint8 *szDID, uint8 *src, uint32 len,uint8 flag, void *arg,gagentUploadDataCb fun )
{
    static uint32_t sn = 0;

    int ret = mqtt_sendGizProtocol2Cloud(getDev2AppTopic(), flag, HI_CMD_PAYLOAD93, sn++, (uint8_t *)src, len );

    fun( ret, arg, szDID );

    return ret;
}
