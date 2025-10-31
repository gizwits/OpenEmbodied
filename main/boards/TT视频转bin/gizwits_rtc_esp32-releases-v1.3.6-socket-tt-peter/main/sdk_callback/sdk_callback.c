// 用于实现客户自身使用的API

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <esp_wifi.h>
#include <inttypes.h>
#include <esp_log.h>
#include "board/charge.h"
#include "cJSON.h"
#include "board/steering_engine.h"
#include "board/gpio.h"

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
#endif

#include "sdk_api.h"
#include "gizwits_product.h"
#include "gizwits_protocol.h"
#include "uart_ctrl_lcd.h"
#include "tt_ledc.h"
#include "gizwits_product.h"
#include "audio_processor.h"

static const char *TAG = "user_api";

// Implementation of notification callbacks
static void app_plugin_notify(char *data) {
    ESP_LOGE(TAG, "on_coze_plugin_notify: %s", data);
    char event_id[32];
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());
    cJSON *root = cJSON_Parse(data);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }
    
    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
    if (!data_obj) {
        ESP_LOGE(TAG, "No data field in JSON");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *req_action = cJSON_GetObjectItem(data_obj, "required_action");
    if (!req_action) {
        ESP_LOGE(TAG, "No required_action field in data");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *submit_tools = cJSON_GetObjectItem(req_action, "submit_tool_outputs");
    if (!submit_tools) {
        ESP_LOGE(TAG, "No submit_tool_outputs field in required_action");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_calls = cJSON_GetObjectItem(submit_tools, "tool_calls");
    if (!tool_calls || cJSON_GetArraySize(tool_calls) == 0) {
        ESP_LOGE(TAG, "No tool_calls array or empty array");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_call = cJSON_GetArrayItem(tool_calls, 0);
    if (!tool_call) {
        ESP_LOGE(TAG, "Failed to get first tool_call");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_call_id = cJSON_GetObjectItem(tool_call, "id");
    if (!tool_call_id || !cJSON_IsString(tool_call_id)) {
        ESP_LOGE(TAG, "No valid id in tool_call");
        cJSON_Delete(root);
        return;
    }

    cJSON *function = cJSON_GetObjectItem(tool_call, "function");
    if (!function) {
        ESP_LOGE(TAG, "No function in tool_call");
        cJSON_Delete(root);
        return;
    }

    cJSON *name = cJSON_GetObjectItem(function, "name");
    if (!name || !cJSON_IsString(name)) {
        ESP_LOGE(TAG, "No valid name in function");
        cJSON_Delete(root);
        return;
    }

    cJSON *arguments = cJSON_GetObjectItem(function, "arguments");
    if (!arguments || !cJSON_IsString(arguments)) {
        ESP_LOGE(TAG, "No valid arguments in function");
        cJSON_Delete(root);
        return;
    }

    // 解析 arguments 字符串，它本身是一个 JSON 字符串
    cJSON *args_json = cJSON_Parse(arguments->valuestring);
    if (!args_json) {
        ESP_LOGE(TAG, "Failed to parse arguments JSON: %s", arguments->valuestring);
        cJSON_Delete(root);
        return;
    }

    
    // 获取conversation_id
    cJSON *conv_id = cJSON_GetObjectItem(data_obj, "conversation_id");
    if (!conv_id || !cJSON_IsString(conv_id)) {
        ESP_LOGE(TAG, "No valid conversation_id");
        cJSON_Delete(args_json);
        cJSON_Delete(root);
        return;
    }
    
    if (strcmp(name->valuestring, "brightness") == 0) {
        cJSON *brightness = cJSON_GetObjectItem(args_json, "brightness");
        if (brightness && cJSON_IsNumber(brightness)) {
            ESP_LOGI(TAG, "Got brightness value: %d", brightness->valueint);
            led_set_global_brightness(brightness->valueint);
            user_storage_save_brightness(brightness->valueint);
            led_effect_stop();
            led_set_state(LED_STATE_STANDBY);
            send_coze_plugin_output_response(event_id, 
                                     cJSON_GetStringValue(conv_id), 
                                     cJSON_GetStringValue(tool_call_id), 
                                     "{\\\"brightness_control_results\\\": \\\"1\\\"}");

        } else {
            ESP_LOGW(TAG, "brightness field not found or not a number");
        }
    } else if (strcmp(name->valuestring, "motor_number") == 0) {
#if defined(CONFIG_SERVO_SUPPORT)
        cJSON *motor_number = cJSON_GetObjectItem(args_json, "motor_number");
        cJSON *motor_frequency = cJSON_GetObjectItem(args_json, "motor_frequency");
        if (motor_number && cJSON_IsNumber(motor_number) && motor_frequency && cJSON_IsNumber(motor_frequency)) {
            ESP_LOGI(TAG, "Got motor_number value: %d, motor_frequency: %d", motor_number->valueint, motor_frequency->valueint);
            wag_tail_start(motor_frequency->valueint, motor_number->valueint);
            send_coze_plugin_output_response(event_id, 
                                     cJSON_GetStringValue(conv_id), 
                                     cJSON_GetStringValue(tool_call_id), 
                                     "{\\\"motor_number_results\\\": \\\"1\\\"}");
        }
#endif
    } else if (strcmp(name->valuestring, "motor_frequency") == 0) {
#if defined(CONFIG_SERVO_SUPPORT)
        cJSON *motor_frequency = cJSON_GetObjectItem(args_json, "motor_frequency");
        if (motor_frequency && cJSON_IsNumber(motor_frequency)) {
            wag_tail_start(WAG_TAIL_GEAR_3_PERIOD, motor_frequency->valueint);
            send_coze_plugin_output_response(event_id, 
                                     cJSON_GetStringValue(conv_id), 
                                     cJSON_GetStringValue(tool_call_id), 
                                     "{\\\"motor_frequency_results\\\": \\\"1\\\"}");
        }
#endif
    } else if (strcmp(name->valuestring, "rgb") == 0) {
        cJSON *r_brightness = cJSON_GetObjectItem(args_json, "r_brightness");
        cJSON *g_brightness = cJSON_GetObjectItem(args_json, "g_brightness");
        cJSON *b_brightness = cJSON_GetObjectItem(args_json, "b_brightness");
        if (r_brightness && cJSON_IsNumber(r_brightness) &&
            g_brightness && cJSON_IsNumber(g_brightness) &&
            b_brightness && cJSON_IsNumber(b_brightness)) {
            ESP_LOGI(TAG, "Got rgb value: %d, %d, %d", r_brightness->valueint, g_brightness->valueint, b_brightness->valueint);
            led_set_rgb(r_brightness->valueint, g_brightness->valueint, b_brightness->valueint);
            user_storage_save_rgb_color(r_brightness->valueint, g_brightness->valueint, b_brightness->valueint);
            send_coze_plugin_output_response(event_id, 
                                     cJSON_GetStringValue(conv_id), 
                                     cJSON_GetStringValue(tool_call_id), 
                                     "{\\\"rgb_control_results\\\": \\\"1\\\"}");
        }
    }
    cJSON_Delete(args_json);
    cJSON_Delete(root);
}

static void app_event_notify(user_event_t event, cJSON *json_data) {
    switch(event) {
        case USER_EVENT_SLEEP:
            ESP_LOGI(TAG, "User event: Sleep");
            battery_state_t state = get_battery_state();
            ESP_LOGW(TAG, "battery state: %d", state);
            if(get_hall_open_once())
            {
                audio_tone_play(1, 1, "spiffs://spiffs/T6_turn_off_96k.mp3"); 
            }

            lcd_state_event_send(EVENT_OFF);

            // 待机不关机
            // if (state == BATTERY_NOT_CHARGING) {
            //     // 电池状态下，直接关机
            //     vTaskDelay(pdMS_TO_TICKS(3000));
            //     gpio_set_power_status(0);
            // } else {
            //     // led_set_state(LED_STATE_SLEEPING);
            //     led_effect_stop();
            // }
            break;
        case USER_EVENT_USER_SPEAKING:
            ESP_LOGI(TAG, "User event: User speaking");
            led_set_state(LED_STATE_USER_SPEAKING);
            lcd_state_event_send(EVENT_LISTEN);
            tt_led_strip_set_state(LED_STATE_OFF);
            break;
        case USER_EVENT_STANDBY:
            ESP_LOGI(TAG, "User event: Standby");
            led_set_state(LED_STATE_STANDBY);
            break;
        case USER_EVENT_WIFI_RECONNECT_FAILED:
            ESP_LOGI(TAG, "User event: WiFi reconnect failed");
            audio_tone_play(1, 0, "spiffs://spiffs/wifi_fail_p_init_2.mp3");
            break;
        case USER_EVENT_CHANGING_AI_AGENT:
            ESP_LOGI(TAG, "User event: Changing AI agent");
            break;
        case USER_EVENT_NET_WORK_ERROR:
            ESP_LOGI(TAG, "User event: Network error");
            led_set_state(LED_STATE_POOR_NETWORK);
            audio_tone_play(1, 0, "spiffs://spiffs/network_error.mp3");
            break;
        case USER_EVENT_SET_VOLUME:
            ESP_LOGI(TAG, "User event: Set volume");
            cJSON *volume = cJSON_GetObjectItem(json_data, "volume");
            if (volume && cJSON_IsNumber(volume)) {
                ESP_LOGI(TAG, "Volume: %d", volume->valueint);
                user_set_volume(volume->valueint);
            }
            break;
        case USER_EVENT_CHAT_IN_PROGRESS:
            if (get_current_actived_mode() == SDK_ACTIVED_MODE_BUTTON) {
                audio_tone_play(0, 0, "spiffs://spiffs/bo.mp3");
            }
            break;
        case USER_EVENT_WAKEUP:
            // 唤醒后请求消息
            set_msg_req(1);
            lcd_state_event_send(EVENT_WAKEUP);
            set_valuestate(state_VALUE2_running);
            set_voice_sleep_flag(false);
            
            if(!get_wakeup_flag())
            {
                // 被唤醒状态下不要这个提示
                // tt_led_strip_set_state(TT_LED_STATE_WHITE);
                audio_tone_play(0, 0, "spiffs://spiffs/T2_positive_feedback_96k.mp3");
            }
            // 最后更新当前唤醒状态
            SET_WAKEUP_FLAG(true);
            break;
        case USER_EVENT_AI_SPEAKING:
            ESP_LOGI(TAG, "User event: AI speaking");
            led_set_state(LED_STATE_AI_SPEAKING);
            lcd_state_event_send(EVENT_REPLY);
            tt_led_strip_set_state(LED_STATE_OFF);
            set_valuestate(state_VALUE2_running);
            break;
        case USER_EVENT_WIFI_INIT:
            ESP_LOGI(TAG, "User event: WiFi initialization");
            tt_led_strip_set_state(TT_LED_STATE_ORANGE);
            break;
        case USER_EVENT_WIFI_DISCONNECTED:
            ESP_LOGI(TAG, "User event: WiFi disconnected");
            led_set_state(LED_STATE_NO_CONNECT_WIFI);
            break;
        case USER_EVENT_WIFI_CONNECTED:
            ESP_LOGI(TAG, "User event: WiFi connected");
            led_set_state(LED_STATE_WIFI_CONNECTED);
            // 重置一下休眠时间
            set_last_audio_time(esp_timer_get_time());

            vTaskDelay(pdMS_TO_TICKS(2000));
            led_set_state(LED_STATE_STANDBY);
            break;
        case USER_EVENT_WIFI_RECONNECTED:
            ESP_LOGI(TAG, "User event: WiFi reconnected");
            led_set_state(LED_STATE_STANDBY);
            break;
        case USER_EVENT_WIFI_CONNECTING:
            led_set_state(LED_STATE_WIFI_CONNECTING);
            audio_tone_play(0, 1, "spiffs://spiffs/converted_startConnect.mp3");
            ESP_LOGI(TAG, "User event: WiFi connecting");
            break;
    }
}

// Initialize callbacks during app startup
void callback_init(void) {
    // Set up callbacks
    sdk_set_coze_plugin_notify_callback(app_plugin_notify);
    sdk_set_user_event_notify_callback(app_event_notify);
}
