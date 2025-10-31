#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <esp_wifi.h>
#include <inttypes.h>

#include "board/gpio.h"
#include "board/charge.h"
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
#include "board/board_init.h"
#include "sdkconfig.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "i2s_stream.h"
#include "periph_adc_button.h"
#include "input_key_service.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "led_strip.h"
#ifdef USE_SDK_LIB
#pragma message( "main USE_SDK_LIB enable!")
#else
#pragma message( "main USE_SDK_LIB disable!")
#include "wifi.h"
#include "mqtt.h"
#include "coze_socket.h"
#include "error_monitor.h"
#include "config.h"
#include "storage.h"
#include "crash_handler.h"
#include "audio_processor.h"

#endif

#include "sdk_api.h"
#include "xtask.h"
#include "hall_switch.h"

static const char* TAG = "VolcRTCApp";
// static StackType_t memory_monitor_task_stack[4096];
// static StaticTask_t memory_monitor_task_buffer;

// TODO 
#ifdef CONFIG_TEST_MODE_VOICE
static void voice_test_task(void *pvParameters) {
    const uint32_t FIRST_INTERVAL_MS = 30000;  // 30秒
    const uint32_t SECOND_INTERVAL_MS = 10000; // 10秒
    
    ESP_LOGI(TAG, "Voice test task started");
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    while (1) {
        // 第一次触发录音
        ESP_LOGI(TAG, "First trigger - starting recording");
        sdk_start_record();
        
        vTaskDelay(pdMS_TO_TICKS(SECOND_INTERVAL_MS));  // 等待10秒
        
        // 第二次触发录音
        ESP_LOGI(TAG, "Second trigger - starting recording");
        sdk_stop_record();

        
        vTaskDelay(pdMS_TO_TICKS(FIRST_INTERVAL_MS));  // 等待30秒后开始下一轮
    }
}
#endif


void dump_debug_info(void) {
    static int count = 0;
    count++;

    wifi_ap_record_t ap_info;
    print_heap_info(TAG, NULL);
    ESP_LOGI(TAG, "=== playing: %d, speaking: %d, sleep_flag: %d, wakeup_flag: %d", 
                get_is_playing_cache(), get_user_speaking_flag(), get_voice_sleep_flag(), get_wakeup_flag());

    if (get_wifi_is_connected() == 1) {
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            int32_t rssi = ap_info.rssi; // 获取信号强度
            ESP_LOGE(TAG,"WiFi Signal RSSI: %d dBm", rssi);
            if (rssi <= -70) {
                led_set_state(LED_STATE_POOR_NETWORK);
            }
        }
    }

    bool not_playing = (audio_tone_url_is_playing() == false && get_is_playing_cache() == false && get_audio_tone_playing() == false);
    audio_sys_get_real_time_stats(count % 50 == 1 && not_playing);
}

void app_main(void)
{
    printf("%s, %d\n", __func__, __LINE__);
    printf("__DATE__: %s __TIME__: %s\n", __DATE__, __TIME__);
    printf("__DATE__: %s __TIME__: %s\n", __DATE__, __TIME__);
    printf("__DATE__: %s __TIME__: %s\n", __DATE__, __TIME__);
    // 初始化主 NVS 分区
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    init_power_hold_gpio(1);
#endif

    esp_err_t ret = nvs_flash_init();
    printf("%s, %d\n", __func__, __LINE__);
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        report_error(ERROR_TYPE_SYSTEM, ERROR_LEVEL_WARNING,
                    "NVS flash needs to be erased", NULL);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        ESP_LOGE(TAG, "nvs_flash_init ret: %d", ret);
    } else {
        ESP_LOGE(TAG, "nvs_flash_init ret: %d", ret);
    }
    // wifi_sdk_init();
    printf("%s, %d\n", __func__, __LINE__);

    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("KEY", ESP_LOG_INFO);
    // esp_log_level_set("nvs", ESP_LOG_DEBUG);

    printf("%s, %d\n", __func__, __LINE__);
    init_board();

    void wakeup_cause_print(void);
    wakeup_cause_print();

    printf("%s, %d\n", __func__, __LINE__);
    callback_init();

    printf("%s, %d\n", __func__, __LINE__);
    storage_init();
    printf("%s, %d\n", __func__, __LINE__);
    storage_load_bootstrap();
    printf("%s, %d\n", __func__, __LINE__);

    factory_test_init();
    printf("%s, %d\n", __func__, __LINE__);

    if (factory_test_is_enabled()) {
        // 产测模式不初始化sdk
        ESP_LOGW(TAG, "Factory test mode is enabled");
        factory_start_lcd_task();
        // 初始化配网
        void wifi_connected_nop(void);
        void wifi_disconnected(void);
        wifi_init(wifi_connected_nop, wifi_disconnected);
    } else if (factory_test_is_aging()) {
        // 老化模式不初始化sdk
        ESP_LOGW(TAG, "Aging test mode is enabled");
        led_set_state(LED_STATE_AGING);
        factory_start_aging_task();
    } else {
        // 初始化sdk
        #if defined(CONFIG_ACTIVED_MODE_BUTTON)
            ESP_LOGI(TAG, "CONFIG_ACTIVED_MODE_BUTTON");
            sdk_init(HARD_VERSION, SOFT_VERSION, SDK_ACTIVED_MODE_BUTTON);
        #elif defined(CONFIG_ACTIVED_MODE_BUTTON_AND_WAKEUP)
            ESP_LOGI(TAG, "CONFIG_ACTIVED_MODE_BUTTON_AND_WAKEUP");
            SET_WAKEUP_FLAG(false);
            sdk_init(HARD_VERSION, SOFT_VERSION, SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP);
        #elif defined(CONFIG_ACTIVED_MODE_SERVER_VAD)
            ESP_LOGI(TAG, "CONFIG_ACTIVED_MODE_SERVER_VAD");
            sdk_init(HARD_VERSION, SOFT_VERSION, SDK_ACTIVED_MODE_SERVER_VAD);
        #elif defined(CONFIG_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP)
            ESP_LOGI(TAG, "CONFIG_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP");
            sdk_init(HARD_VERSION, SOFT_VERSION, SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP);
        #else
            ESP_LOGI(TAG, "CONFIG_ACTIVED_MODE_BUTTON DEFAULT");
            sdk_init(HARD_VERSION, SOFT_VERSION, SDK_ACTIVED_MODE_BUTTON);
        #endif

            bool is_config_wifi = get_is_config_wifi();
            bool is_connected_wifi = get_wifi_is_connected();
            if(is_config_wifi == false) {
                user_set_volume(DEFAULT_LOGIC_VOLUME);
                if(get_hall_state() == HALL_STATE_OFF)
                {
                    // 开盖时会在开盖回调触发
                    audio_tone_play(1, 1, "spiffs://spiffs/go_to_config.mp3");
                }
                led_set_state(LED_STATE_INIT);
            } else if (is_connected_wifi == false) {
                led_set_state(LED_STATE_NO_CONNECT_WIFI);
            }
            // todo Add custom functions from here
        #ifdef CONFIG_TEST_MODE_VOICE
            xTaskCreate(voice_test_task, "voice_test_task", 2048, NULL, 5, NULL);
        #endif
    }

    printf("%s, %d\n", __func__, __LINE__);
    void memory_monitor_task(void *pvParameters);

    // xTaskCreateExt(memory_monitor_task, "memory_monitor", 4096, NULL, 5, NULL);

    wifi_ap_record_t ap_info;
    while (0) {
        print_heap_info(TAG, NULL);
        ESP_LOGI(TAG, "playing: %d, speaking: %d, sleep_flag: %d, wakeup_flag: %d", 
                 get_is_playing_cache(), get_user_speaking_flag(), get_voice_sleep_flag(), get_wakeup_flag());

        if (get_wifi_is_connected() == 1) {
             if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                int32_t rssi = ap_info.rssi; // 获取信号强度
                ESP_LOGE(TAG,"WiFi Signal RSSI: %d dBm", rssi);
                if (rssi <= -70) {
                    led_set_state(LED_STATE_POOR_NETWORK);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5*1000));
    }

    vTaskDelete(NULL);
}


#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
// #include "esp_spiram.h"
#include "esp_chip_info.h"
#include "esp_log.h"

// static const char *TAG = "MemoryDemo";

void memory_monitor_task(void *pvParameters)
{
    while (1) {
        // ESP_LOGI(TAG, "\n===== ESP32-S3 Memory Monitor =====");
        
        // // 1. Print chip info
        // esp_chip_info_t chip_info;
        // esp_chip_info(&chip_info);
        // ESP_LOGI(TAG, "\n[Chip Information]");
        // ESP_LOGI(TAG, "  Model: %s", "ESP32-S3");
        // ESP_LOGI(TAG, "  Cores: %d", chip_info.cores);
        // ESP_LOGI(TAG, "  Revision: r%d", chip_info.revision);

        // 2. Print memory information
        ESP_LOGI(TAG, "\n[Memory Information]");
        
        // Internal RAM
        size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        
        ESP_LOGI(TAG, "  Internal RAM:");
        ESP_LOGI(TAG, "    Total: %d bytes (%.2f KB)", total_internal, total_internal / 1024.0);
        ESP_LOGI(TAG, "    Free: %d bytes (%.2f KB)", free_internal, free_internal / 1024.0);
        ESP_LOGI(TAG, "    Minimum free: %d bytes", min_free_internal);
        ESP_LOGI(TAG, "    Usage: %.2f%%", (1 - (float)free_internal / total_internal) * 100);

        // External PSRAM (if enabled)
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        
        ESP_LOGI(TAG, "  External PSRAM:");
        ESP_LOGI(TAG, "    Total: %d bytes (%.2f MB)", total_psram, total_psram / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "    Free: %d bytes (%.2f MB)", free_psram, free_psram / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "    Minimum free: %d bytes", min_free_psram);
        ESP_LOGI(TAG, "    Usage: %.2f%%", (1 - (float)free_psram / total_psram) * 100);

        // // 3. Print detailed heap information
        // ESP_LOGI(TAG, "\n[Heap Details]");
        // heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        // heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);

        // // 4. Print task information
        // ESP_LOGI(TAG, "\n[Task Information]");
        // printf("Task Name\tStatus\tPrio\tHWM\tTask#\tAffinity\n");

        // // Print current task stack high water mark
        // ESP_LOGI(TAG, "\n[Current Task Stack]");
        // UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
        // ESP_LOGI(TAG, "  Stack high water mark: %d bytes", watermark);

        // // 5. System memory summary
        // ESP_LOGI(TAG, "\n[System Memory Summary]");
        // ESP_LOGI(TAG, "  Free heap: %d bytes", esp_get_free_heap_size());
        // ESP_LOGI(TAG, "  Minimum free heap: %d bytes", esp_get_minimum_free_heap_size());

        // Print every 5 seconds
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// void app_main(void)
// {
//     // Initialize logging
//     esp_log_level_set(TAG, ESP_LOG_INFO);

//     // Check PSRAM initialization
//     if (esp_spiram_is_initialized()) {
//         ESP_LOGI(TAG, "External PSRAM initialized, Size: %d MB", esp_spiram_get_size() / (1024 * 1024));
//     } else {
//         ESP_LOGW(TAG, "External PSRAM not initialized or unavailable");
//     }

//     // Create memory monitor task
//     xTaskCreate(memory_monitor_task, "memory_monitor", 4096, NULL, 5, NULL);

//     // Main task can continue other work...
//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(10000));
//     }
// }