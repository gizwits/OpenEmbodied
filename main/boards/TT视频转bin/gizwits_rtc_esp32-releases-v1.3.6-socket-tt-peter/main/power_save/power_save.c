#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"  // 添加头文件以使用pdMS_TO_TICKS
#include "hal/rtc_io_hal.h"
#include "power_save.h"
#include "gizwits_protocol.h"
#include "uart_ctrl_lcd.h"

#define HALL_WAKEUP_GPIO_NUM    HALL_SENSOR_GPIO        // 用于HALL唤醒的GPIO
#define POWER_WAKEUP_GPIO_NUM   get_input_play_id()     // 用于POWER唤醒的KEY1 GPIO

void wakeup_cause_print(void);

void deep_sleep_enter_standby(void)
{
    ESP_LOGI("SLEEP", "Preparing to enter deep sleep --- standby");

    // 配置唤醒引脚
    rtc_gpio_init(HALL_WAKEUP_GPIO_NUM);
    rtc_gpio_set_direction(HALL_WAKEUP_GPIO_NUM, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(HALL_WAKEUP_GPIO_NUM);
    rtc_gpio_pullup_dis(HALL_WAKEUP_GPIO_NUM);
    esp_sleep_enable_ext1_wakeup_io(1ULL << HALL_WAKEUP_GPIO_NUM, ESP_EXT1_WAKEUP_ANY_HIGH);  // 高电平唤醒

    // 配置唤醒引脚
    rtc_gpio_init(POWER_WAKEUP_GPIO_NUM);
    rtc_gpio_set_direction(POWER_WAKEUP_GPIO_NUM, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(POWER_WAKEUP_GPIO_NUM);
    rtc_gpio_pullup_dis(POWER_WAKEUP_GPIO_NUM);
    esp_sleep_enable_ext0_wakeup(POWER_WAKEUP_GPIO_NUM, 0);  // 低电平唤醒

    ESP_LOGW("SLEEP", "=== Holding power rtcio -- standby --, GPIO%d", get_power_hold_gpio());
    rtc_gpio_hold_en(get_power_hold_gpio());

    ESP_LOGW("SLEEP", "=== Entering deep sleep -- standby --, waiting for hall GPIO%d, power GPIO%d to wake up...", HALL_WAKEUP_GPIO_NUM, POWER_WAKEUP_GPIO_NUM);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_deep_sleep_start();  // 启动深度睡眠
    // esp_light_sleep_start();  // 启动浅度睡眠
}

// void deep_sleep_enter_close(void)
// {
//     ESP_LOGI("SLEEP", "Preparing to enter deep sleep --- close");

//     // 配置唤醒引脚
//     esp_sleep_enable_ext0_wakeup(POWER_WAKEUP_GPIO_NUM, 0);  // 低电平唤醒

//     // // 配置为输入模式 + 下拉（若接按钮）
//     gpio_pad_select_gpio(POWER_WAKEUP_GPIO_NUM);
//     // gpio_set_direction(POWER_WAKEUP_GPIO_NUM, GPIO_MODE_INPUT);
//     // gpio_pullup_dis(POWER_WAKEUP_GPIO_NUM);
//     // gpio_pulldown_en(POWER_WAKEUP_GPIO_NUM);

//     gpio_set_direction(POWER_WAKEUP_GPIO_NUM, GPIO_MODE_INPUT);
//     gpio_pullup_en(POWER_WAKEUP_GPIO_NUM);     // 打开内部上拉
//     gpio_pulldown_dis(POWER_WAKEUP_GPIO_NUM);  // 禁用下拉

//     ESP_LOGW("SLEEP", "Entering deep sleep -- close --, waiting for GPIO%d to wake up...", POWER_WAKEUP_GPIO_NUM);
//     vTaskDelay(pdMS_TO_TICKS(3000));
//     esp_deep_sleep_start();  // 启动深度睡眠
// }

#define SLEEP_DELAY_MS 6000
#define SLEEP_DELAY_MS_STEP 100
#define SLEEP_DELAY_CNT (SLEEP_DELAY_MS / SLEEP_DELAY_MS_STEP)

// 休眠唤醒原因为ext0（power)唤醒，等待6秒:
//     如果6秒内触发关机事件，则进行关机；
//     如果6秒内触发开盖事件，则按步骤1运行；6秒内未触发关机事件，则重新进入休眠；
void wakeup_by_power_key(void)
{
    bool is_hall_wakeup = false;
    int hall_cnt = 0;
    int power_key_cnt = 0;


    gpio_set_direction(HALL_SENSOR_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HALL_SENSOR_GPIO, GPIO_PULLUP_ONLY);

    gpio_set_direction(POWER_WAKEUP_GPIO_NUM, GPIO_MODE_INPUT);
    gpio_set_pull_mode(POWER_WAKEUP_GPIO_NUM, GPIO_PULLUP_ONLY);

    is_hall_wakeup = false;
    for (int i = 0; i < SLEEP_DELAY_CNT; i++) {
        if ((i%3) == 0) {
            lcd_state_event_send(EVENT_OFF);
        }
        vTaskDelay(pdMS_TO_TICKS(SLEEP_DELAY_MS_STEP));
        if (1 == gpio_get_level(HALL_SENSOR_GPIO)) {
            hall_cnt ++;
            if (hall_cnt >= 3) {
                // 如果6秒内触发开盖事件，则按步骤1运行
                ESP_LOGW("SLEEP", "=== Hall wakeup, hall_cnt: %d", hall_cnt);
                is_hall_wakeup = true;
                lcd_state_event_send(EVENT_INIT);
                audio_tone_play(1, 1, "spiffs://spiffs/converted_turn_on.mp3");
                break;
            }
        } else {
            hall_cnt = 0;
        }

        if (0 == gpio_get_level(POWER_WAKEUP_GPIO_NUM)) {
            power_key_cnt ++;
            if (power_key_cnt >= 15) {
                // 如果6秒内触发开盖事件，则按步骤1运行
                ESP_LOGW("SLEEP", "=== Power key off, cnt: %d", power_key_cnt);
                while(0 == gpio_get_level(POWER_WAKEUP_GPIO_NUM)) {
                    printf(".");
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                ESP_LOGW("SLEEP", "=== Power off after power key released");
                close_device(false);
                break;
            }
        } else {
            power_key_cnt = 0;
        }
    }

    if (!is_hall_wakeup && get_valuestate() != state_VALUE0_close) {
        // 6秒内未触发开盖事件、关机事件，则重新进入休眠
        deep_sleep_enter_standby();
    }
}

void wakeup_cause_print(void)
{

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char* wakeup_reason_str[] = {
        "ESP_SLEEP_WAKEUP_UNDEFINED",    // In case of deep sleep, reset was not caused by exit from deep sleep
        "ESP_SLEEP_WAKEUP_ALL",          // Not a wakeup cause, used to disable all wakeup sources with esp_sleep_disable_wakeup_source
        "ESP_SLEEP_WAKEUP_EXT0",         // Wakeup caused by external signal using RTC_IO
        "ESP_SLEEP_WAKEUP_EXT1",         // Wakeup caused by external signal using RTC_CNTL
        "ESP_SLEEP_WAKEUP_TIMER",        // Wakeup caused by timer
        "ESP_SLEEP_WAKEUP_TOUCHPAD",     // Wakeup caused by touchpad
        "ESP_SLEEP_WAKEUP_ULP",          // Wakeup caused by ULP program
        "ESP_SLEEP_WAKEUP_GPIO",         // Wakeup caused by GPIO (light sleep only on ESP32, S2 and S3)
        "ESP_SLEEP_WAKEUP_UART",         // Wakeup caused by UART (light sleep only)
        "ESP_SLEEP_WAKEUP_WIFI",         // Wakeup caused by WIFI (light sleep only)
        "ESP_SLEEP_WAKEUP_COCPU",        // Wakeup caused by COCPU int
        "ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG", // Wakeup caused by COCPU crash
        "ESP_SLEEP_WAKEUP_BT"            // Wakeup caused by BT (light sleep only)
    };

    if (cause < sizeof(wakeup_reason_str) / sizeof(wakeup_reason_str[0])) {
        printf("=== Wakeup reason: %s\n", wakeup_reason_str[cause]);
    } else {
        printf("=== Wakeup reason: Unknown\n");
    }

    switch (cause)
    {
    case ESP_SLEEP_WAKEUP_EXT0:
        // 休眠唤醒原因为ext0（power)唤醒，等待6秒:
        wakeup_by_power_key();
        break;
    
    case ESP_SLEEP_WAKEUP_EXT1:
        // 休眠唤醒原因为ext1（hall）唤醒，则正常启动
        // PASS Through
    
    default:
        // 非休眠唤醒，正常启动
        lcd_state_event_send(EVENT_INIT);
        audio_tone_play(1, 1, "spiffs://spiffs/converted_turn_on.mp3");
        break;
    }

    hall_sensor_init();
}



// 外部供电（深睡还需要吗？）
void external_power_supply_demo(void)
{
    // 设置 GPIO4 为输出高电平
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 1);

    // 这里LCD IO是否需要锁存
    // 使能 GPIO hold 功能
    gpio_hold_en(GPIO_NUM_4);                 // 锁存当前电平状态
    gpio_deep_sleep_hold_en();                // 使能所有锁存功能在 deep sleep 时生效

    // 可选：保持 RTC 外设供电（某些场景下需要）
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    printf("Entering deep sleep, GPIO4 will remain high...\n");
    esp_deep_sleep_start();
}
