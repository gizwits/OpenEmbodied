// 从app_main.c剥离，封装初始化核心业务

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <esp_wifi.h>
#include <inttypes.h>

#include "wifi/ap_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_task_info.h"
#include "esp_random.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "sdkconfig.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "esp_netif.h"
#include "i2s_stream.h"
#include "periph_adc_button.h"
#include "periph_spiffs.h"
#include "input_key_service.h"
#include "driver/sdmmc_host.h"
#include "sdk_init.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "gizwits_protocol.h"
#include "gizwits_product.h"
#include "hall_switch.h"
#include "board/charge.h"

#ifdef USE_SDK_LIB
#pragma message( "main USE_SDK_LIB enable!")
#include "sdk_dependency.h"
#else
#pragma message( "main USE_SDK_LIB disable!")
#include "wifi.h"
#include "mqtt.h"
#include "coze_socket.h"
#include "error_monitor.h"
#include "config.h"
#include "storage.h"
#include "crash_handler.h"
#endif

#include "sdk_api.h"
#include "audio_log.h"
#include "uart/uart_ctrl_lcd.h"
#include "tt_ledc.h"

#define STATS_TASK_PRIO         5


static const char* TAG = "sdkService";
static int is_wifi_connected = 0;

static StackType_t *audio_timeout_task_stack = NULL;
static TaskHandle_t audio_timeout_check_task_handle = NULL;
static StaticTask_t audio_timeout_task_buffer;

static char trace_id[37];

audio_board_handle_t audio_board_handle = NULL;


// 添加一个全局变量来存储当前的激活模式
static sdk_actived_mode_t current_actived_mode = SDK_ACTIVED_MODE_SERVER_VAD;

// 提供一个获取当前激活模式的函数
sdk_actived_mode_t get_current_actived_mode(void) {
    return current_actived_mode;
}

static void http_callback(mqtt_config_t *config)
{
    ESP_LOGI(TAG, "run mqtt_init");
    // 连接 mqtt
    mqtt_init(NULL, config);
}

static void bootstrap_http_callback(mqtt_config_t *config){
    http_callback(config);
    // 写入数据 已经授权
    storage_save_bootstrap(1);
}

static void gatOnboarding_task(void *pvParameter)
{
    gatOnboarding(bootstrap_http_callback);
    vTaskDelete(NULL);
}

static void gatProvision_task(void *pvParameter)
{
    gatProvision(http_callback);
    vTaskDelete(NULL);

}

static void wifi_connected(void)
{
    configure_dns();
    ble_stop();
    socket_handler_init(get_current_actived_mode());


    is_wifi_connected = 1;
    bool has_bootstrap = storage_load_bootstrap();

    if (!has_bootstrap) {
        ESP_LOGI(TAG, "No DID found, starting onboarding");
        // 调用注册接口 传入 callback
        // 启动一个线程调用
        xTaskCreate(&gatOnboarding_task, "gatOnboarding", 4096, NULL, STATS_TASK_PRIO, NULL);
    } else {
        // 启动一个线程调用
        xTaskCreate(&gatProvision_task, "gatProvision", 4096, NULL, STATS_TASK_PRIO, NULL);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    audio_tone_stop();
}

void wifi_connected_nop(void)
{
    is_wifi_connected = 1;
}

void wifi_disconnected(void)
{
    is_wifi_connected = 0;
    // tt_led_strip_set_state(TT_LED_STATE_ORANGE); // todo peter mark ?
}

void run_sleep_task() {
    run_sleep();
    vTaskDelete(NULL);
}

void run_sleep(void) {
    static volatile bool is_processing_sleep = false;

    if (is_processing_sleep) {
        // ESP_LOGI(TAG, "Sleep is already processing, exiting task");
        return;
    }
    is_processing_sleep = true;

    #ifdef NO_SLEEP
    // 不处理休眠
    return;
    #endif
    
    // 熄灭屏幕
    // 发送熄灭事件到C3
#if defined(CONFIG_TT_MUSIC_HW_1V5)
    if(get_valuestate() != state_VALUE0_close && get_hall_state() == HALL_STATE_ON)
#endif
    {
        if (get_valuestate() == state_VALUE0_close) {
            ESP_LOGI(TAG, "MagicI is close, exiting task");
            is_processing_sleep = false;
            return;
        }
        if (get_voice_sleep_flag() && !get_wakeup_flag()) {
            // ESP_LOGI(TAG, "Sleep flag is set, exiting task");
            is_processing_sleep = false;
            return;
        }
    }
    ESP_LOGI(TAG, "%s: processing=%d, state=%d, hall=%d, sleep=%d", __func__, is_processing_sleep, get_valuestate(), get_hall_state(), get_voice_sleep_flag());

    SET_WAKEUP_FLAG(false);
    set_voice_sleep_flag(true);

#if defined(CONFIG_TT_MUSIC_HW_1V5)
    if (get_valuestate() != state_VALUE0_close)
#endif
    {
        set_valuestate(state_VALUE1_standby);
    }

    lcd_state_event_send(EVENT_OFF);
    
    current_led_update();
    user_event_notify(USER_EVENT_SLEEP);
    // tt_led_strip_set_state(TT_LED_STATE_ORANGE); // todo peter mark ?

#if defined(CONFIG_TT_MUSIC_HW_1V5)
    ESP_LOGW(TAG, "=== %s: state=%d, hall=%d, sleep=%d, wakeup=%d, wifi=%d", __func__, get_valuestate(), get_hall_state(), get_voice_sleep_flag(), get_wakeup_flag(), storage_has_wifi_config());
    if (get_valuestate() == state_VALUE0_close 
        || (storage_has_wifi_config() && !battery_is_usb_plugged() && get_hall_state() == HALL_STATE_OFF)) {
        // 真待机
        ESP_LOGW(TAG, "Stop es7210 and enter deep sleep...");
        es7210_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if(storage_has_wifi_config() && !battery_is_usb_plugged() && get_hall_state() == HALL_STATE_OFF) {
        // 只有在已配网、未插USB、且合盖的情况下，才进入休眠
        deep_sleep_enter_standby();
    } else {
        // 假待机
        ESP_LOGW(TAG, "=== Standby only, not sleep ===");
    }

    is_processing_sleep = false;
    
#else
    // 假待机
    is_processing_sleep = false;
#endif

}

static void audio_timeout_check_task(void *pvParameters) {
    // ESP_LOGE(TAG, "Starting audio timeout check task");

    while (true) {
        int sleep_timeout = SLEEP_TIME;
        if (is_websocket_connected() == false) {
            // 没有连ws的情况下，延长休眠时间
            sleep_timeout = 180 * 1000 * 1000;
        }

        if (get_voice_sleep_flag()) {
            // ESP_LOGI(TAG, "Sleep flag is set, exiting task");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }


        if (get_is_playing_cache()) {
            // ESP_LOGI(TAG, "Agent is speaking, updating last audio time");
            set_last_audio_time(esp_timer_get_time());
            vTaskDelay(pdMS_TO_TICKS(300));  // 更频繁地更新时间
            continue;
        }

        uint32_t current_time = esp_timer_get_time();
        
#if defined(CONFIG_SUPPORT_HEART_BEAT)
        static uint32_t last_heart_beat_time = 0;
        ESP_LOGI(TAG, "current_time: %lu, last_audio_time: %lu", 
                 (unsigned long)current_time, 
                 (unsigned long)current_time - get_last_audio_time());
                 
        if (get_last_audio_time() > 0 && current_time - get_last_audio_time() > 15 * 1000 * 1000) {
            // 当前如果用户在说话或者 ai 在说话，则不发送
            // 云端 vad 的情况下，用户是否在说话 不用判断，但是这个暂时不用管
            ESP_LOGI(TAG, "is_playing_cache: %d, recording_flag: %d", get_is_playing_cache(), get_user_speaking_flag());

            set_last_audio_time(esp_timer_get_time());

            if (!get_is_playing_cache() && !get_user_speaking_flag()) {
                ESP_LOGI(TAG, "15s 发送一次问候");
                send_heart_beat_message(last_heart_beat_time);
                last_heart_beat_time += 1;
            } else {
                ESP_LOGI(TAG, "忽略心跳");
            }
        }
#endif
#ifndef NO_SLEEP

// 临时代码  心跳模式不休眠
#if !defined(CONFIG_SUPPORT_HEART_BEAT)
        // ESP_LOGI(TAG, "last_audio_time: %lu, current_time: %lu, offset_time: %lu", get_last_audio_time(), current_time, current_time - get_last_audio_time());
        if (!audio_tone_url_is_playing() && get_last_audio_time() > 0 && current_time - get_last_audio_time() > sleep_timeout && !get_voice_sleep_flag() ) {
            ESP_LOGI(TAG, "Audio timeout detected");
            run_sleep();
        }
#endif
#endif
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "Audio timeout check task ending");
    vTaskDelete(NULL);
    audio_timeout_check_task_handle = NULL;
}


// 添加 DNS 配置函数
void configure_dns(void) {
    // 配置国内常用 DNS 服务器（按优先级排序）
    // ip_addr_t dns_servers[3];
    
    // 阿里 DNS（通常速度最快）
    // IP_ADDR4(&dns_servers[0], 223, 5, 5, 5);      // 主：阿里 DNS
    // IP_ADDR4(&dns_servers[1], 223, 6, 6, 6);      // 备：阿里 DNS

    //  IP_ADDR4(&dns_servers[0], 1, 1, 1, 1);      // cf DNS
    // IP_ADDR4(&dns_servers[1], 1, 0, 0, 1);      // cf DNS

    // IP_ADDR4(&dns_servers[2], 8, 8, 8, 8);// 8.8.8.8 DNS
    
    // 设置 DNS 服务器
    // dns_setserver(0, &dns_servers[0]);  // 主 DNS
    // dns_setserver(1, &dns_servers[1]);  // 第一备用
    // dns_setserver(2, &dns_servers[2]);  // 第二备用 DNS
}

// 修改 sdk_init 函数定义，增加激活模式参数
void sdk_init(const char* hard_version, const char* soft_version, sdk_actived_mode_t actived_mode)
{
    // 保存激活模式
    current_actived_mode = actived_mode;
    
    // 创建 trace id
    sdk_version_init(hard_version, soft_version);

    gen_trace_id();

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t esp_set = esp_periph_set_init(&periph_cfg);


    // storage_init();
    // storage_load_bootstrap();

#if defined(CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
    // initAudioBuffer();
    audio_log_init();
#endif
    // 初始化错误监控
    error_monitor_init();

    // last do it
    void devAuthResultCb(void);
    devAuthResultCb();

    // 初始化配网

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("RTC_HANDLER", ESP_LOG_INFO);
    esp_log_level_set("audio processor", ESP_LOG_INFO);
    esp_log_level_set("mqtt_example", ESP_LOG_INFO);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_DEBUG);

    // 测试模式不创建休眠检测任务
#if !defined(CONFIG_TEST_MODE_VOICE)
    audio_timeout_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_timeout_task_stack) {
        report_error(ERROR_TYPE_MEMORY, ERROR_LEVEL_ERROR,
                    "Failed to allocate audio timeout task stack in PSRAM",
                    "app_main.c:audio_timeout_check_task");
        return false;
    }
    // 创建休眠检测任务
    audio_timeout_check_task_handle = xTaskCreateStaticPinnedToCore(
        &audio_timeout_check_task,
        "audio_timeout_check_task",
        4 * 1024,
        NULL,
        5,
        audio_timeout_task_stack,
        &audio_timeout_task_buffer,
        0
    );
#endif

#if defined(CONFIG_RTC_AUDIO_SAVE_TO_SERVER)
    startAudioUploadService();
#endif

    wifi_init(wifi_connected, wifi_disconnected);

    char mac_str[13];
    get_mac_str(mac_str, true);
    printf("MAC address: %s\n", mac_str);
    printf("Version: %s \n", sdk_get_software_version());

}

/**
 * @brief Generate a random trace ID (UUID v4 format)
 * 
 * This function generates a random UUID v4 format string that can be used
 * as a trace ID for logging and debugging purposes.
 * 
 * @param trace_id Buffer to store the generated trace ID
 * @param size Size of the buffer (should be at least 37 bytes for full UUID)
 * @return ESP_OK if successful, otherwise an error code
 */
void gen_trace_id() {
    uint8_t random_bytes[16];
    
    // Generate 16 random bytes
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    // Set version to 4 (random UUID)
    random_bytes[6] = (random_bytes[6] & 0x0F) | 0x40;
    
    // Set variant to RFC4122
    random_bytes[8] = (random_bytes[8] & 0x3F) | 0x80;
    
    // Format as UUID string: 8-4-4-4-12 hex digits
    snprintf(trace_id, sizeof(trace_id),
             "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             random_bytes[0], random_bytes[1], random_bytes[2], random_bytes[3],
             random_bytes[4], random_bytes[5],
             random_bytes[6], random_bytes[7],
             random_bytes[8], random_bytes[9],
             random_bytes[10], random_bytes[11], random_bytes[12], 
             random_bytes[13], random_bytes[14], random_bytes[15]);
    
    return ESP_OK;
}

char *get_trace_id() {
    return trace_id;
}