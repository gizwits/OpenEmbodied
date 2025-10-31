/* UART Echo Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include <string.h>
#include "eye_lcd.h"
#include "button.h"

/**
 * This is an example which echos any data it receives on configured UART back to the sender,
 * with hardware flow control turned off. It does not use UART driver event queue.
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */

#define ECHO_TEST_TXD (CONFIG_EXAMPLE_UART_TXD)
#define ECHO_TEST_RXD (CONFIG_EXAMPLE_UART_RXD)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (CONFIG_EXAMPLE_UART_PORT_NUM)
#define ECHO_UART_BAUD_RATE     (CONFIG_EXAMPLE_UART_BAUD_RATE)
#define ECHO_TASK_STACK_SIZE    (CONFIG_EXAMPLE_TASK_STACK_SIZE)

bool is_on_screen = false;

static const char *TAG = "UART TEST";

#define BUF_SIZE (1024)

static void echo_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
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
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif


    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, data, (BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        // Write data back to the UART
        uart_write_bytes(ECHO_UART_PORT_NUM, (const char *) data, len);
        if (len) {
            data[len] = '\0';
            if(strncmp((char *) data, "left", strlen("left")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_direction(1);
                ESP_LOGI(TAG, "Command matched: left");
            }
            else if(strncmp((char *) data, "right", strlen("right")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_direction(-1);
                ESP_LOGI(TAG, "Command matched: right");
            }
            else if(strncmp((char *) data, "reverse", strlen("reverse")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                reverse_direction();
                ESP_LOGI(TAG, "Command matched: reverse");
            }
            else if(strncmp((char *) data, "reply", strlen("reply")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_video(VIDEO_REPLY);
                ESP_LOGI(TAG, "Command matched: reply");
            }
            else if(strncmp((char *) data, "think", strlen("think")) == 0)  
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_video(VIDEO_THINK);
                ESP_LOGI(TAG, "Command matched: think");
            }
            else if(strncmp((char *) data, "wakeup", strlen("wakeup")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_video(VIDEO_WAKEUP);
                ESP_LOGI(TAG, "Command matched: wakeup");
            }
            else if(strncmp((char *) data, "listen", strlen("listen")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_video(VIDEO_LISTEN);
                ESP_LOGI(TAG, "Command matched: listen");
            }
            else if(strncmp((char *) data, "off", strlen("off")) == 0)
            {
                if (is_on_screen) {
                    turn_off_screen();
                    is_on_screen = false;
                }
                set_video(VIDEO_OFF);
                ESP_LOGI(TAG, "Command matched: off");
            }
            else if(strncmp((char *) data, "factory_test", strlen("factory_test")) == 0)
            {
                if (!is_on_screen) {
                    turn_on_screen();
                    is_on_screen = true;
                }
                set_video(VIDEO_FACTORY_TEST);
                ESP_LOGI(TAG, "Command matched: factory_test");
            }
            else if(strncmp((char *) data, "red", strlen("red")) == 0)
            {
                set_factory_color(FACTORY_COLOR_RED);
                set_video(VIDEO_SET_COLOR);
                ESP_LOGI(TAG, "Command matched: red");
            }
            else if(strncmp((char *) data, "green", strlen("green")) == 0)
            {
                set_factory_color(FACTORY_COLOR_GREEN);
                set_video(VIDEO_SET_COLOR);
                ESP_LOGI(TAG, "Command matched: green");
            }   
            else if(strncmp((char *) data, "blue", strlen("blue")) == 0)
            {
                set_factory_color(FACTORY_COLOR_BLUE);
                set_video(VIDEO_SET_COLOR);
                ESP_LOGI(TAG, "Command matched: blue");
            }
            else if(strncmp((char *) data, "yellow", strlen("yellow")) == 0)
            {
                set_factory_color(FACTORY_COLOR_YELLOW);
                set_video(VIDEO_SET_COLOR);
                ESP_LOGI(TAG, "Command matched: yellow");
            }

            // ESP_LOGI(TAG, "Recv str: %s", (char *) data);
            // // 打印十六进制数据
            // ESP_LOGI(TAG, "Hex dump:");
            // for(int i = 0; i < len; i++) {
            //     printf("%02x ", data[i]);
            // }
            // printf("\n");
            
        }
    }
}

// 按钮回调函数
static void button_callback(button_event_t event)
{
    static video_type_t current_type = 0;  // 初始状态为待机动画
    
    switch(event) {
        case BUTTON_EVENT_PRESSED:
            ESP_LOGI("BUTTON", "Button clicked");
            set_video(current_type);
            current_type += 1;

            if (current_type >= VIDEO_OFF)
            {
                current_type = VIDEO_THINK;
            }
            break;
        
        default:
            break;
    }
}

void uart_echo_app_main(void)
{
    // 初始化按钮
    button_config_t button_config = {
        .long_press_time = 2000,    // 长按2秒
        .double_click_time = 300    // 双击间隔300ms
    };
    // ESP_ERROR_CHECK(board_button_init(42, &button_config, button_callback));

    xTaskCreate(echo_task, "uart_echo_task", ECHO_TASK_STACK_SIZE, NULL, 10, NULL);
}
