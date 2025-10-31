// 实现二开客户所需的API

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
#include "sdk_init.h"
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
#include "esp_http_client.h"
#include "esp_timer.h"
#include "led_strip.h"
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
#include "sdk_version.h"
#include "sdk_callback.h"

#include "audio_processor.h"


/* 重置模组（清除wifi信息和解除绑定关系) */
void sdk_wifi_reset( void )
{
    mqtt_sendReset2Cloud();
    storage_reset_wifi_config();
    esp_restart();
}


int sdk_get_volume() {
    return sdk_volume;
}

bool get_is_config_wifi(void) {
    return storage_load_bootstrap_cache();
}

bool get_ai_is_playing(void) {
    return get_is_playing_cache();
}

bool get_wifi_is_connected(void) {
    return get_esp_wifi_is_connected();
}

void send_coze_plugin_output_response(char *event_id, char *conv_id, char *tool_call_id, char *data) {
    send_tool_output_response(event_id, conv_id, tool_call_id, data);
}

void sdk_start_record(void) {
    SET_WAKEUP_FLAG(true);
    xTaskCreatePinnedToCore(start_recorder_with_key, "start_recorder_with_key", 1024 * 4, NULL, 10, NULL, 0);
}

void sdk_stop_record(void) {
    xTaskCreatePinnedToCore(end_recorder_task, "end_recorder_task", 1024 * 4, NULL, 2, NULL, 0);
}

void sdk_break_record(void) {
    xTaskCreatePinnedToCore(break_rec_with_key, "break_rec_with_key", 1024 * 4, NULL, 2, NULL, 0);
}

const char* sdk_get_hardware_version(void) {
    return sdk_version_get_hardware();
}

const char* sdk_get_software_version(void) {
    return sdk_version_get_software();
}

void sdk_set_coze_plugin_notify_callback(coze_plugin_notify_cb cb) {
    set_coze_plugin_notify_callback(cb);
}

void sdk_set_user_event_notify_callback(user_event_notify_cb cb) {
    set_user_event_notify_callback(cb);
}


#if defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
void sdk_volume_cycle_control(void) {
    static bool is_increasing = true;  // 用于跟踪音量变化方向
    
    if (is_increasing) {
        // 音量递增模式
        if (user_get_volume() >= LOGIC_MAX_VOLUME) {
            // 达到最大音量,切换为递减模式
            is_increasing = false;
            user_set_volume_sub(10);
        } else {
            user_set_volume_add(10);
        }
    } else {
        // 音量递减模式 
        if (user_get_volume() <= LOGIC_MIN_VOLUME) {
            // 达到最小音量,切换为递增模式
            is_increasing = true;
            user_set_volume_add(10);
        } else {
            user_set_volume_sub(10);
        }
    }

    // 播放提示音
    if (!get_ai_is_playing()) {
        if (user_get_volume() >= LOGIC_MAX_VOLUME) {
            audio_tone_play(1, 0, "spiffs://spiffs/max_voice.mp3");
        } else if (user_get_volume() <= LOGIC_MIN_VOLUME) {
            audio_tone_play(1, 0, "spiffs://spiffs/min_voice.mp3"); 
        } else {
            audio_tone_play(1, 0, "spiffs://spiffs/cur_vol_.mp3");
        }
    }
}
#endif
