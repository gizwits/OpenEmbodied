#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "uart_ctrl_lcd.h"
#include "hall_switch.h"
#include "gizwits_product.h"
#include "gizwits_protocol.h"
#include "config.h"
#include "coze_socket.h"
#include "config/config.h"
#include "tt_ledc.h"
#include "power_save.h"
#include "esp_log.h"
#include "battery.h"
#include "board/charge.h"

#define HALL_DEBOUNCE_MS 30 // 防止开盖不利索，导致关盖误触打断push语音
#define CHECK_INTERVAL_MS 5
uint8_t is_hall_sensor_init = 0;
static uint8_t hall_open_once = 0;
uint8_t get_hall_open_once(void)
{
    return hall_open_once;
}

static const char *TAG = "HALL_SENSOR";


static hall_state_t current_hall_state = HALL_STATE_OFF;
static uint32_t stable_time = 0;
static TimerHandle_t hall_timer;
static SemaphoreHandle_t hall_mutex = NULL;


uint8_t __get_hall_state(const char* fun, int32_t line) {
    uint8_t state = current_hall_state;

    return state;
}

void hall_timer_callback() {
    static int8_t last_hall_state = -1;  // 增加上一次状态变量
    if(is_hall_sensor_init == 0) {
    printf("%s %d \n", __func__, __LINE__);
        return;
    }
    if(get_valuestate() == state_VALUE0_close)
    {
#if !defined(CONFIG_TT_MUSIC_HW_1V5)
        last_hall_state = -1;
        current_hall_state = HALL_STATE_OFF;
#else
        if (!battery_is_usb_plugged()) {
            // 检测到关机状态，且未处于充电，则进行关机
            void close_device(bool is_delayed);
            close_device(true);
        }
#endif
    // printf("Hall sensor state changed to ON, but state_VALUE0_close \n");
        return;
    } else if(get_valuestate() == state_VALUE1_standby) {
        if (storage_has_wifi_config() && !battery_is_usb_plugged()) {
            // 检测到关机状态，且未处于充电，则进行关机
            run_sleep();
        }
    }

    int level = gpio_get_level(HALL_SENSOR_GPIO);
    uint32_t current_tick = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (level == current_hall_state) {
        if(stable_time == 0)
        {
            stable_time = current_tick;  // 重置稳定时间
            printf("%s %d stable_time %d \n", __func__, __LINE__, stable_time);
        }

    } 
    else
    {
        stable_time = 0;
        current_hall_state = level;
    }

    // if(current_tick%100 == 0)
    // {
    //     printf("%s %d current_tick %d stable_time %d \n", __func__, __LINE__, current_tick, stable_time);
    //     printf("%s %d l:%d,c:%d\n", __func__, __LINE__,last_hall_state,current_hall_state);
    // }

    if (current_tick - stable_time >= HALL_DEBOUNCE_MS) 
    {
        // current_hall_state = level;
        if(last_hall_state != current_hall_state) 
        {
            if(last_hall_state != current_hall_state)
            printf("%s %d l:%d,c:%d\n", __func__, __LINE__,last_hall_state,current_hall_state);
        
#if !defined(CONFIG_TT_MUSIC_HW_1V5)
            set_valuestate(current_hall_state == HALL_STATE_OFF ? state_VALUE0_close : state_VALUE2_running);
#else
            set_valuestate(current_hall_state == HALL_STATE_OFF ? state_VALUE1_standby : state_VALUE2_running);
#endif
            system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
            printf("Hall sensor state changed to %s from %s \n", 
                    current_hall_state == HALL_STATE_ON ? "ON" : "OFF",
                    last_hall_state == HALL_STATE_ON ? "ON" : "OFF");

            last_hall_state = current_hall_state;  // 更新上一次状态
            if(current_hall_state == HALL_STATE_ON)
            {
                hall_open_once = 1;
                hall_on_print();
                reset_wifi_restart_flag();

                set_last_audio_time(esp_timer_get_time());
                lcd_state_event_send(EVENT_WAKEUP);// 有时序要求
                                // 被唤醒状态下不要这个提示
                // tt_led_strip_set_state(TT_LED_STATE_WHITE);

                SET_WAKEUP_FLAG(true);
                set_voice_sleep_flag(false);
                vTaskDelay(pdMS_TO_TICKS(1000));
                

                // 异步可能出两次语音
                // system_os_post(USER_TASK_PRIO_2, MSG_HALL_ON_PLAY, 0);

                if( get_onboarding_on())
                {
                    // 有概率关盖声音还在继续
                    // vTaskDelay(pdMS_TO_TICKS(500));
                    tt_led_strip_set_state(TT_LED_STATE_ORANGE);
                    audio_tone_play(1, 1, "spiffs://spiffs/go_to_config.mp3");
                }
                else
                {
                    audio_tone_play(1, 1, "spiffs://spiffs/T2_positive_feedback_96k.mp3");
                }

            }
            else
            {
                hall_off_print();
                void clear_audio_buffer(void);
                clear_audio_buffer();
            }
        }
        if(current_hall_state == HALL_STATE_OFF)
        {
            if(get_wakeup_flag())
            {
                SET_WAKEUP_FLAG(false);
                run_sleep();
            }

            // // todo 真待机
            // ESP_LOGW(TAG, "Stop es7210 and enter deep sleep...");
            // es7210_stop();
            // vTaskDelay(pdMS_TO_TICKS(100));
            // deep_sleep_enter();
        }
    }
}

void hall_timer_task(void *pvParameters) {
    static int cnt = 0;

    while (1) {
        hall_timer_callback();
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
        cnt ++;
        if (cnt % 1000 == 0) {
            printf("%s running, cnt: %d ms\n",__func__, cnt*CHECK_INTERVAL_MS);
        }
    }
}

void hall_sensor_init() {
    // if (hall_mutex == NULL) {
    //     hall_mutex = xSemaphoreCreateMutex();
    // }
    is_hall_sensor_init = 1;
    gpio_set_direction(HALL_SENSOR_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HALL_SENSOR_GPIO, GPIO_PULLUP_ONLY);
    TaskHandle_t hall_timer_task_handle = xTaskCreate(
        hall_timer_task, 
        "HALL_TIMER_TASK", 
        4 * 1024, 
        NULL, 
        15, 
        NULL
    );
}


void hall_on_print()
{

    char mac_str[13];
    get_mac_str(mac_str, true);
    product_info_t *pInfo = get_product_info();
    wifi_ap_record_t ap_info;
    int32_t rssi;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi; // 获取信号强度
    }
    else
    {
        rssi = 0;
    }
    printf("\n\n\n\n\n\n\n\n\n\n\n"
           "   |*****************************\n"
           "   |HALL ON\n"
           "   |version:%s\n"
           "   |BAT:%d\n"
           "   |mac:%s,did:%s\n"
           "   |WIFI-RSSI:%d\n"
           "   |hardware:%s\n"
           "   |*****************************\n"
           "\n\n\n\n\n\n\n\n\n\n\n",
           SOFT_VERSION, battery_get_estimate(TYPE_AVERAGE), mac_str, pInfo->szDID, rssi, HARD_VERSION);
}

void hall_off_print()
{
    printf("\n\n\n\n\n\n\n\n\n\n\n"
           "   |*****************************\n"
           "   |HALL OFF\n"
           "   |*****************************\n"
           "\n\n\n\n\n\n\n\n\n\n\n");
}

