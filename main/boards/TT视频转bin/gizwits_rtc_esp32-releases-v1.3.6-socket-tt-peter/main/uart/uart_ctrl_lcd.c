/* UART Echo Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include "uart_ctrl_lcd.h"
#include "sdkconfig.h"
#include "coze_socket.h"
#include "hall_switch.h"
// #include "config.h"
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1 ) || defined( CONFIG_AUDIO_BOARD_ATOM_V1_2) // CONFIG_AUDIO_BOARD_ATOM_V1_2 test
#pragma message("/root/gizwits_rtc_esp32/main/uart/uart_ctrl_lcd.c CONFIG_AUDIO_BOARD_TT_MUSIC_V1")

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
/**
 * This is an example which processes specific commands received on configured UART,
 * with hardware flow control turned off. It uses a queue to handle events.
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: on
 * - Pin assignment: see defines below (See Kconfig)
 */

#define ECHO_TEST_TXD (3)
#define ECHO_TEST_RXD (UART_PIN_NO_CHANGE)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (2)
#define ECHO_UART_BAUD_RATE     (115200)
#define ECHO_TASK_STACK_SIZE    (1024*4)    // 别动栈，会crash

// static StaticTask_t uart_event_task_buffer;
// static StackType_t uart_event_task_stack[ECHO_TASK_STACK_SIZE];

static const char *TAG = "UART_CTRL_LCD";

const char *EVENT_LIST[] = {
    STR_EVENT_REPLY,
    STR_EVENT_THINK,
    STR_EVENT_WAKEUP,
    STR_EVENT_LISTEN,
    STR_EVENT_OFF,
    STR_EVENT_FACTORY_TEST,
    STR_EVENT_RED,
    STR_EVENT_GREEN,
    STR_EVENT_BLUE,
    STR_EVENT_YELLOW,
    STR_EVENT_INIT,
};

#define BUF_SIZE (1024)
#define QUEUE_SIZE 10

static QueueHandle_t uart_event_queue;
static uint8_t last_event = 0xff;  // 使用数组来存储上一个事件
static void process_event(event_type_t event)
{
    if (event >= EVENT_LIST_SIZE || event < 0) {
        ESP_LOGW(TAG, "%s error ! received a NULL event %d", __func__, event);
        return;
    }

    // 检查是否为新事件
    bool is_new_event = (last_event == 0xff) || (last_event != event);

    ESP_LOGI(TAG, "%s %d event: %s %s --- last %s ", \
        __func__, __LINE__, is_new_event ? "is new" : "is old", 
        EVENT_LIST[event], last_event == 0xff ? "none" : EVENT_LIST[last_event]);
    
    if (!is_new_event) {

        // vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (is_new_event) {
        int ret = uart_write_bytes(ECHO_UART_PORT_NUM, (uint8_t *)EVENT_LIST[event], strlen(EVENT_LIST[event]));
        ESP_LOGI(TAG, "uart_write_bytes ret: %d", ret);
    }
    
    // 更新上一个事件
    last_event = event;
    // 如果上一个事件是EVENT_WAKEUP，延迟两秒再结束线程
    if (last_event == EVENT_WAKEUP) {
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}


static void uart_event_task(void *arg)
{
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
#pragma message("CONFIG_UART_ISR_IN_IRAMCONFIG_UART_ISR_IN_IRAMCONFIG_UART_ISR_IN_IRAM")
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    if (uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE, 0, 0, NULL, intr_alloc_flags) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
    }
    if (uart_param_config(ECHO_UART_PORT_NUM, &uart_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
    }
    if (uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
    }

    uint8_t event = 0;  // 使用固定大小的数组来存储事件
    while (1) {
        if (xQueueReceive(uart_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event >= sizeof(EVENT_LIST) / sizeof(EVENT_LIST[0])) {
                ESP_LOGE(TAG, "Received invalid event: %d", event);
                continue;
            }
            if (event < 0) {
                ESP_LOGE(TAG, "Received negative event: %d", event);
                continue;
            }
            // ESP_LOGI(TAG, "%s %d event %d, %s", __func__, __LINE__, event, EVENT_LIST[event]);
            process_event(event);
            vTaskDelay(pdMS_TO_TICKS( event == EVENT_OFF? 10 :100));// 让C3串口超时不粘包
            //  ESP_LOGI(TAG, "%s %d event %d", __func__, __LINE__, event);
            
        }
    }
}

void uart_ctrl_lcd_task_init(void)
{
    uart_event_queue = xQueueCreate(QUEUE_SIZE, sizeof(uint8_t));  // 修改队列中元素的大小
    if (uart_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UART event queue");
        return;
    }

    if (xTaskCreate(uart_event_task, "uart_event_task", ECHO_TASK_STACK_SIZE, NULL, 15, NULL) == NULL) {
        ESP_LOGE(TAG, "Failed to create UART event task");
    }
}

void __lcd_state_event_send(const char * func, uint32_t line, event_type_t event)
{
    static uint8_t first_wakeup = 0;
    static uint8_t e = 0;

    static int64_t last_event_time = 0;


    // C3可能被插电重启, 还是每4秒允许被重发一些状态
    if (last_event != 0xff) {
        if (last_event_time == 0) {
            last_event_time = xTaskGetTickCount();
        } else {
            uint32_t current_time = xTaskGetTickCount();
            if ((current_time - last_event_time) > pdMS_TO_TICKS(4000)) { // 超过4秒
                last_event_time = 0; // 清除时间记录
                last_event = 0xff;
            }
        }
    }

    if(event != EVENT_OFF && last_event == event)
    {
        return;
    }
    
    if (event >= sizeof(EVENT_LIST) / sizeof(EVENT_LIST[0])) {
        ESP_LOGE(TAG, "Invalid event to send: %d", event);
        return;
    }

    if(get_hall_state() == HALL_STATE_OFF && event != EVENT_OFF && event != EVENT_INIT)
    {
        ESP_LOGE(TAG, "HALL_STATE_OFF, event %d change to OFF", event);
        event = EVENT_OFF;
    }

    if ( factory_test_is_enabled() || factory_test_is_aging() )
    {
        if(event != EVENT_RED && event != EVENT_GREEN && event != EVENT_BLUE && event != EVENT_YELLOW)
        {
            // ESP_LOGI(TAG, "%s factory mode no Send event: %s, by %s:%d", __func__, EVENT_LIST[event], func, line);
            return;
        }
    }

    if(get_wakeup_flag() && event == EVENT_WAKEUP)
    {
        // if(first_wakeup != 0)   // 开机第一次唤醒需要发
        // {
            ESP_LOGI(TAG, "%s wakeup mode no Send event: %s, by %s:%d", __func__, EVENT_LIST[event], func, line);
            return;
        // }
        // else
        // {
        //     first_wakeup = 1;
        // }
    }

    // 初始化不判断其他状态直接发
    if(event == EVENT_INIT)
    {
        event = EVENT_WAKEUP;
    }

    e = event;

    ESP_LOGI(TAG, "%s Send event: %s, by %s:%d", __func__, EVENT_LIST[event], func, line);
    if (uart_event_queue) {
        if (xQueueSend(uart_event_queue, &e, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to send event to queue");
        }
    }
}
#else
#define lcd_state_event_send(a) {}
#define uart_ctrl_lcd_task_init() {}
#endif