#include "input_key_service.h"
#include "esp_wifi.h"
// #include "config.h"
#include "board/gpio.h"
#include "esp_log.h"
#include "board_init.h"
#include "button.h"
#include "error_monitor.h"
#include "board/charge.h"
#include "factory_test/factory_test.h"
#include "audio_log.h"
#include "tt_ledc.h"
#include "uart_ctrl_lcd.h"
#include "gizwits_protocol.h"
#include "coze_socket.h"
#include "hall_switch.h"
#include "gizwits_product.h"
#include "gizwits_protocol.h"
#include "es7210.h"
#include "wifi.h"
#include "ble.h"
#include "mqtt.h"
#include "audio_processor.h"

#define CONFIG_AUDIO_BOARD_TT_MUSIC_V1


static TaskHandle_t *resetTaskHandle = NULL;

static const char *TAG = "KEY";

// __attribute__((weak)) void key_on_rec_break_pressed(void)
// {
//     // Weak function, do nothing
// }

void close_device(bool is_delayed)
{
    static volatile bool is_processing = false;
    if (is_processing) {
        return;
    }
    is_processing = true;

    ESP_LOGW(TAG, "=== Close device: delayed=%s ===", is_delayed ? "true" : "false");

    // void clear_audio_buffer(void);
    // clear_audio_buffer();


    ESP_LOGW(TAG, "Closeing device...");

    // 下面本地语音播放的qos为1 假如url播放未结束会死等结束出音效
    // 先设置状态为关闭，给url播放线程识别退出
    set_valuestate(state_VALUE0_close); 
    if (!is_delayed) {
        audio_tone_play(1, 1, "spiffs://spiffs/T6_turn_off_96k.mp3");
    }
    
    tt_led_strip_set_state(TT_LED_STATE_OFF);
    lcd_state_event_send(EVENT_OFF);
    
// #if defined(CONFIG_TT_MUSIC_HW_1V3)

    if(get_socket_client() != NULL)
    {
        printf("Closed the cover, disconnected ws\n");
        set_manual_break_flag(false);
        set_need_switch_socket_room(false);
        // leave_room();
        cleanup_websocket();
        set_room_info_request_id(xTaskGetTickCount()+battery_get_voltage());
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGW(TAG, "Key CLOSE MagicI");
    // recorder_pipeline_stop();
    // set_manual_break_flag(1);
    // es7210_adc_set_gain(ES7210_INPUT_MIC1, GAIN_0DB);// todo Peter mark mic麦克风增益
    // es7210_adc_set_gain(ES7210_INPUT_MIC2, GAIN_0DB);
    // 保证音频播放
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!battery_is_usb_plugged()) {
        ESP_LOGW(TAG, "=== Power off now ===");
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_power_status(0);
    } else {
        ESP_LOGW(TAG, "=== Will Power off after usb unpluged ===");
        // vTaskDelay(pdMS_TO_TICKS(1000));
        // run_sleep();    // 这里有音效
        // gpio_set_ext_power_en(0);
    }
    
    set_wakeup_flag(false);
    set_voice_sleep_flag(true);

    if (!is_delayed) {
        mqtt_deinit();
        app_wifi_deinit();
        ble_stop_advertising();
        // ble_stop();
        // stop_recorder_pipeline();
    }

// #elif defined(CONFIG_TT_MUSIC_HW_1V5)

//     run_sleep();

// #else
// #error "CONFIG_TT_MUSIC_HW_1V3 or CONFIG_TT_MUSIC_HW_1V5 must be defined"
// #endif

    is_processing = false;
}

void handle_power_off_logic() 
{
    ESP_LOGI(TAG, "get_valuestate():%d", get_valuestate());
    // 关机逻辑
    if(get_valuestate() == state_VALUE2_running || get_valuestate() == state_VALUE1_standby) {
        close_device(false);
    }
    // else if(get_hall_state() == HALL_STATE_ON) {
    //     // 开机逻辑
    //     gpio_set_power_status(1);
    //     gpio_set_ext_power_en(1);
    //     set_valuestate(state_VALUE2_running);
    //     ESP_LOGI(TAG, "Key OPEN  MagicI");
    //     set_msg_req(1);
    //     vTaskDelay(pdMS_TO_TICKS(500));
    //     lcd_state_event_send(EVENT_WAKEUP);
    //     tt_led_strip_set_state(TT_LED_STATE_WHITE);
    //     set_voice_sleep_flag(false);
    //     SET_WAKEUP_FLAG(true);

    //     if(get_socket_client() == NULL) {
    //         mqtt_sem_give();
    //     }
    // }
    else
    {
        esp_restart();
    }
}
// 按键事件处理函数
void key_event_callback(key_event_t event)
{
#ifdef CONFIG_FACTORY_TEST_MODE_ENABLE
#warning "factory test mode enabled in key.c"
    if (factory_test_is_enabled()) {
        // 产测模式下，不响应按键
        ESP_LOGI(TAG, "key_event_callback: Factory test mode, ignore event: %d", event);
        return;
    } else {
        // 处理进入/退出退出模式
        switch (event) {
            case KEY_EVENT_REC_BREAK:
                // 录音按键短按
                key_on_rec_break_pressed();
                break;
            default:
                break;
        }
    }
#endif
    
    ESP_LOGI(TAG, "key_event_handler event: %d", event);
 

    if(get_is_config_wifi() == false && (event != KEY_EVENT_POWER_OFF && event != KEY_EVENT_RESET)) {
        // 没有配置Wi-Fi，提示错误
        // 获取上电时间（毫秒）
        uint32_t power_on_time = esp_timer_get_time() / 1000;
        if (power_on_time < 10000) {  // 10秒 = 10000毫秒
            return;
        }
        user_set_volume(80);
        audio_tone_play(1, 1, "spiffs://spiffs/go_to_config.mp3");
        return;
    }

    if (get_esp_wifi_is_connected() == false) {
        // 获取上电时间（毫秒）
        uint32_t power_on_time = esp_timer_get_time() / 1000;
        if (power_on_time < 10000) {  // 10秒 = 10000毫秒
            return;
        }
        // 没有连接Wi-Fi，提示错误
        switch (event) {
            // 只受理重置
            case KEY_EVENT_POWER_OFF:
                handle_power_off_logic();
                break;
            case KEY_EVENT_RESET:
                audio_tone_play(0, 1, "spiffs://spiffs/reset_success.mp3");
                vTaskDelay(pdMS_TO_TICKS(1100));
                set_onboarding_on(1);   // 避免在重启前wifi离线报错
                led_set_state(LED_STATE_RESET);
                vTaskDelay(pdMS_TO_TICKS(2000));
                sdk_wifi_reset();
                break;
            case KEY_EVENT_ONBOARDING:
                ESP_LOGW(TAG, "KEY_EVENT_ONBOARDING");
                audio_tone_play(0, 1, "spiffs://spiffs/T2_positive_feedback_96k.mp3");
                // 进入配网 
                set_onboarding_on(1);   // 避免在重启前wifi离线报错
                storage_save_onboarding_flag(true);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
                break;
            default:
                // user_set_volume(80);
                // audio_tone_play(0, 1, "spiffs://spiffs/wifi_fail_p_init.mp3");
                break;
        }
        return;
    }

    switch (event) {
        case KEY_EVENT_POWER_OFF:
            handle_power_off_logic();
            break;
        case KEY_EVENT_REC_BREAK:
            sdk_break_record();
            break;
        case KEY_EVENT_REC_START:
            sdk_start_record();
            led_set_state(LED_STATE_USER_SPEAKING);
            lcd_state_event_send(EVENT_LISTEN);
            break;
        case KEY_EVENT_REC_STOP:
            sdk_stop_record();
            led_set_state(LED_STATE_STANDBY);
            lcd_state_event_send(EVENT_THINK);
            break;
        case KEY_EVENT_RESET:
            audio_tone_play(0, 1, "spiffs://spiffs/reset_success.mp3");
            led_set_state(LED_STATE_RESET);
            vTaskDelay(pdMS_TO_TICKS(2000));
            sdk_wifi_reset();
            break;
        case KEY_EVENT_VOLUME_UP:
            vTaskDelay(pdMS_TO_TICKS(50));
            if (user_get_volume() >= LOGIC_MAX_VOLUME) {
                // 提示已经最大了
                if (!get_ai_is_playing() && !get_audio_url_is_playing()) {
                    audio_tone_play(0, 1, "spiffs://spiffs/max_voice.mp3");
                }
            } else {
                user_set_volume_add(5);
                if (!get_ai_is_playing() && !get_audio_url_is_playing()) {
                    audio_tone_play(0, 1, "spiffs://spiffs/cur_vol_.mp3");
                }
            }
            break;
        case KEY_EVENT_VOLUME_DOWN:
            vTaskDelay(pdMS_TO_TICKS(50));
            if (user_get_volume() <= 40) {
                // 提示已经最小了
                if (!get_ai_is_playing() && !get_audio_url_is_playing()) {
                    audio_tone_play(0, 1, "spiffs://spiffs/min_voice.mp3");
                }
            } else {
                user_set_volume_sub(5);
                if (!get_ai_is_playing() && !get_audio_url_is_playing()) {
                    audio_tone_play(0, 1, "spiffs://spiffs/cur_vol_.mp3");
                }
            }
            break;
// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
//         case KEY_EVENT_AUDIO_LOG:
//         {
//             static bool is_log_inited = false;
//             ESP_LOGE(TAG, "KEY_EVENT_AUDIO_LOG");
//             if (!is_log_inited) {
//                 audio_log_init();
//                 is_log_inited = true;
//             } else {
//                 audio_log_end();
//                 audio_log_disable();
//                 is_log_inited = false;
//             }
//             break;
//         }
// #endif
        case KEY_EVENT_TEST_LED:
            ESP_LOGW(TAG, "KEY_EVENT_TEST_LED");
            led_set_state(LED_STATE_TEST_LED);
            break;
        case KEY_EVENT_ONBOARDING:
            ESP_LOGW(TAG, "KEY_EVENT_ONBOARDING");
            audio_tone_play(0, 1, "spiffs://spiffs/bo.mp3");
            // 进入配网 
            storage_save_onboarding_flag(true);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            break;
        default:
            ESP_LOGW(TAG, "Unknown key event: %d", event);
            break;
    }
}


#ifdef CONFIG_ESP32_S3_BOX_3_BOARD
static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    ESP_LOGD(TAG, "[ * ] input key id is %d, %d", (int)evt->data, evt->type);
    const char *key_types[INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE + 1] = {"UNKNOWN", "CLICKED", "CLICK RELEASED", "PRESSED", "PRESS RELEASED"};
    
    if (key_event_callback == NULL) {
        return ESP_OK;
    }

    // 任意事件都可以解除休眠
    key_event_callback(KEY_EVENT_SLEEP);

    switch ((int)evt->data) {
        case INPUT_KEY_USER_ID_REC:
            ESP_LOGI(TAG, "[ * ] [Rec] KEY %s", key_types[evt->type]);
            if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK || evt->type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE) {
                // 触发休眠事件
                key_event_callback(KEY_EVENT_REC);
            }
            break;
        case INPUT_KEY_USER_ID_PLAY:
            if (evt->type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE) {
                // 触发重置事件
                key_event_callback(KEY_EVENT_RESET);
            }
            ESP_LOGI(TAG, "[ * ] [Play] KEY %s", key_types[evt->type]);
            break;
    }

    return ESP_OK;
}
#endif

void run_effect_with_index(int index) {
#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    led_color_t color = get_key_led_color(index);
    led_effect_msg_t msg = {
        .effect = LED_EFFECT_MARQUEE,
        .config.marquee = {
            .r = color.r,
            .g = color.g,
            .b = color.b,
            .start_pos = key_to_led_position(index),
            .rounds = 4,
            .delay_ms = 100
        }
    };
    led_effect_start(&msg);
#endif
}

// 音量减
static bool is_long_up_pressed = false;
static bool is_long_down_pressed = false;

static void button_up_event_handler(button_event_t event) {
    // ESP_LOGE(TAG, "button_up_event_handler");
    switch (event) {
        case BUTTON_EVENT_CLICKED:
        case BUTTON_EVENT_RELEASED:
            is_long_up_pressed = false;
            break;
        case BUTTON_EVENT_PRESSED:
            is_long_up_pressed = false;
            run_effect_with_index(INPUT_KEY_USER_ID_VOLUP);
            key_event_callback(KEY_EVENT_SLEEP);
            key_event_callback(KEY_EVENT_VOLUME_UP);
            ESP_LOGI(TAG, "Button clicked");
            break;
        case BUTTON_EVENT_LONG_PRESSED:
            is_long_up_pressed = true;
        #if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
            if (is_long_down_pressed && is_long_up_pressed) {
                key_event_callback(KEY_EVENT_RESET);
            }
        #endif
            break;
    }
}
static void button_down_event_handler(button_event_t event) {
    switch (event) {
        case BUTTON_EVENT_CLICKED:
        case BUTTON_EVENT_RELEASED:
            is_long_down_pressed = false;
            break;
        case BUTTON_EVENT_PRESSED:
            is_long_down_pressed = false;
            run_effect_with_index(INPUT_KEY_USER_ID_VOLDOWN);
            key_event_callback(KEY_EVENT_SLEEP);
            key_event_callback(KEY_EVENT_VOLUME_DOWN);
            ESP_LOGI(TAG, "Button clicked");
            break;
        case BUTTON_EVENT_LONG_PRESSED:
            is_long_down_pressed = true;
        #if defined(CONFIG_AUDIO_BOARD_ATOM_V1)
            // v1 长按- 直接关机
            key_event_callback(KEY_EVENT_RESET);
        #endif

        #if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
            if (is_long_down_pressed && is_long_up_pressed) {
                key_event_callback(KEY_EVENT_RESET);
            } else {
                key_event_callback(KEY_EVENT_AUDIO_LOG);
            }
        #endif
            break;
    }
}


static void button_rec_event_handler(button_event_t event) {
    ESP_LOGE(TAG, "button_rec_event_handler: %d", event);
    switch (event) {
        // case BUTTON_EVENT_TRIPLE_CLICKED:
        //     // 触发个空指针异常
        //     ESP_LOGE(TAG, "Button triple clicked");
        //     int *p = NULL;
        //     *p = 1;
        //     break;
        case BUTTON_EVENT_PRESSED:
            // 录音有别的灯效
            ESP_LOGE(TAG, "KEY_EVENT_REC_BREAK");
            key_event_callback(KEY_EVENT_SLEEP);
            key_event_callback(KEY_EVENT_REC_BREAK);
            lcd_state_event_send(EVENT_LISTEN);
        case BUTTON_EVENT_CLICKED:
            // vTaskDelay(pdMS_TO_TICKS(500));
            // led_set_state(LED_STATE_STANDBY);
            break;
        case BUTTON_EVENT_LONG_PRESSED:
            ESP_LOGI(TAG, "KEY_EVENT_REC_START");
            key_event_callback(KEY_EVENT_REC_START);
            lcd_state_event_send(EVENT_LISTEN);
            break;
        case BUTTON_EVENT_RELEASED:
            ESP_LOGI(TAG, "KEY_EVENT_REC_STOP");
            key_event_callback(KEY_EVENT_REC_STOP);
            lcd_state_event_send(EVENT_THINK);
            break;
    }
}

static void button_reset_event_handler(button_event_t event) {
    // ESP_LOGE(TAG, "button_up_event_handler");
    switch (event) {
        case BUTTON_EVENT_PRESSED:
            key_event_callback(KEY_EVENT_SLEEP);
            ESP_LOGI(TAG, "%s %d Button clicked", __FUNCTION__, __LINE__);
            break;
        case BUTTON_EVENT_LONG_PRESSED:
            ESP_LOGI(TAG, "%s %d Button long pressed", __FUNCTION__, __LINE__);
            key_event_callback(KEY_EVENT_RESET);
            break;
    }
}


#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
static uint8_t click_count = 0;
static uint32_t last_power_press_time = 0;
#define POWER_BUTTON_RESET_TIMEOUT_MS 3000  // 3秒内需要完成5次按键

// static uint8_t green = 160; 
// static uint8_t blue = 118; 

// uint8_t get_green()
// {
//     return green;
// }

// uint8_t get_blue()
// {
//     return blue;
// }
static esp_timer_handle_t reset_timer = NULL;
static void reset_timer_callback(void* arg) {
    key_event_callback(KEY_EVENT_RESET);
    click_count = 0;  // 重置计数
}
const esp_timer_create_args_t reset_timer_args = {
    .callback = (esp_timer_cb_t)reset_timer_callback,
    .arg = NULL,
    .name = "reset_timer"
};

static esp_timer_handle_t onboarding_timer = NULL;
static void onboarding_timer_callback(void* arg) {
    key_event_callback(KEY_EVENT_ONBOARDING);
    click_count = 0;  // 重置计数
}
const esp_timer_create_args_t onboarding_timer_args = {
    .callback = (esp_timer_cb_t)onboarding_timer_callback,
    .arg = NULL,
    .name = "onboarding_timer"
};

void timer_start(esp_timer_handle_t *timer, const esp_timer_create_args_t *timer_args, int32_t active_time_ms) {
    if (*timer == NULL) {
        ESP_LOGI(TAG, "Creating and starting reset timer for %d ms", active_time_ms);
        esp_timer_create(timer_args, timer);
        esp_timer_stop(*timer);
        esp_timer_start_once(*timer, active_time_ms * 1000);  // 2秒后触发
    } else {
        ESP_LOGI(TAG, "Reset timer already exists, restarting for 2 seconds");
        esp_timer_stop(*timer);
        esp_timer_start_once(*timer, active_time_ms * 1000);  // 2秒后触发
    }
}
void timer_stop_delete(esp_timer_handle_t *timer) {
    if (*timer != NULL) {
        ESP_LOGI(TAG, "stopping and deleting reset timer %p", *timer);
        esp_timer_stop(*timer);
        esp_timer_delete(*timer);
        *timer = NULL;
    }
}


static void button_power_event_handler(button_event_t event) {
    ESP_LOGI(TAG, "button_power_event_handler: %d", event);
    uint32_t current_time = esp_timer_get_time() / 1000;  // 转换为毫秒

    // static uint8_t color;
    switch (event) {
        case BUTTON_EVENT_PRESSED:
            run_effect_with_index(INPUT_KEY_USER_ID_PLAY);
            
#if 1
            // 检查是否在超时时间内
            if (current_time - last_power_press_time > POWER_BUTTON_RESET_TIMEOUT_MS) {
                // 超时，重置计数
                click_count = 0;
            }
            
            // 更新最后按键时间
            last_power_press_time = current_time;
            
            // 增加计数
            click_count++;
            ESP_LOGI(TAG, "Power button pressed %d times, interval:%d", click_count, current_time - last_power_press_time);
            
            // green -= 5;
            // ESP_LOGI(TAG, "green %d blue %d", green, blue);
            // tt_led_strip_set_state(TT_LED_STATE_WHITE);

            // 检查是否达到5次
            if(click_count >= 2)
            {
                // user_set_volume(DEFAULT_LOGIC_VOLUME);
                // #if defined(CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
                // #warning "RTC_AUDIO_SAVE_TO_FLASH enabled in button_mode_event_handler"
                //         // FIXME：屏蔽测试代码
                //         audio_log_end();
                // #endif
            }

            // if (click_count >= 5) {
            //     ESP_LOGI(TAG, "Power button pressed 5 times, triggering reset");
            //     key_event_callback(KEY_EVENT_RESET);
            //     click_count = 0;  // 重置计数
            // }

            ESP_LOGI(TAG, "Click count: %d", click_count);

            // Peter Mark 打断url播放测试
            // if(click_count == 2)
            // {
            //     set_audio_url_is_failed(1);
            // }
            
            if (click_count == 3) {
                // 如果已经有定时器存在，先重新计算
                timer_start(&onboarding_timer, &onboarding_timer_args, 1500);
            }
            if(click_count > 3)
            {
                timer_stop_delete(&onboarding_timer);
            }

            // 检查是否达到5 - 7次点击
            if (click_count >= 5 && click_count <= 7) {
                // 如果已经有定时器存在，先重新计算

                timer_start(&reset_timer, &reset_timer_args, 2000);
            }
            if(click_count > 7)
            {

                timer_stop_delete(&reset_timer);
            }

            if(click_count >= 10) {
                // 15次点击后，进入老化测试模式
                handle_aging_test_mode();
                click_count = 0;
            }
#else
            void key_on_rec_break_pressed(void);
            key_on_rec_break_pressed();
#endif
            break;
            
        case BUTTON_EVENT_LONG_PRESSED:
            key_event_callback(KEY_EVENT_POWER_OFF);
            click_count = 0;  // 长按也重置计数
            
            break;
    }
}
static void button_mode_event_handler(button_event_t event) {
    ESP_LOGE(TAG, "button_mode_event_handler");
    switch (event) {
    case BUTTON_EVENT_PRESSED:
        ESP_LOGW(TAG, "KEY_EVENT_MODE_CHANGE");
#if defined(CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
#warning "RTC_AUDIO_SAVE_TO_FLASH enabled in button_mode_event_handler"
        // FIXME：屏蔽测试代码
        audio_log_end();
#endif
        
        // blue -= 5;
        // ESP_LOGI(TAG, "green %d blue %d", green, blue);
        // tt_led_strip_set_state(TT_LED_STATE_WHITE);

        // key_event_callback(KEY_EVENT_SLEEP);
        // key_event_callback(KEY_EVENT_TEST_LED);
        break;
    case BUTTON_EVENT_LONG_PRESSED:
        ESP_LOGW(TAG, "KEY_EVENT_MODE_LONG_PRESS, triggering reset");
        key_event_callback(KEY_EVENT_RESET);
        break;
    }
}
#endif
#if defined(CONFIG_AUDIO_BOARD_ATOM_V1)
static void button_power_event_handler(button_event_t event) {
    static uint8_t click_count = 0;
    static int64_t last_click_time = 0;
    const int64_t click_timeout_ms = 2000; // 1秒内的点击才计数
    switch (event) {
        case BUTTON_EVENT_DOUBLE_CLICKED:
            ESP_LOGI(TAG, "KEY_EVENT_POWER_OFF");
            key_event_callback(KEY_EVENT_POWER_OFF);
            break;
        case BUTTON_EVENT_PRESSED:
            ESP_LOGI(TAG, "KEY_EVENT_REC_BREAK");
            key_event_callback(KEY_EVENT_SLEEP);
            key_event_callback(KEY_EVENT_REC_BREAK);
            break;
        case BUTTON_EVENT_LONG_PRESSED:
            ESP_LOGI(TAG, "KEY_EVENT_REC_START");
            key_event_callback(KEY_EVENT_REC_START);
            break;
        case BUTTON_EVENT_RELEASED:
            ESP_LOGI(TAG, "KEY_EVENT_REC_STOP");
            key_event_callback(KEY_EVENT_REC_STOP);
            break;
        case BUTTON_EVENT_TRIPLE_CLICKED:
            ESP_LOGI(TAG, "Triple click detected");
            // 在这里处理三击事件
            key_event_callback(KEY_EVENT_POWER_OFF);
            break;
    }
}

#endif

void key_init()
{
#if defined(CONFIG_AUDIO_BOARD_ATOM_V1)
    // gpio 按钮
    button_config_t config_quick = BUTTON_DEFAULT_CONFIG();
    config_quick.long_press_time = 600;
    button_config_t config = BUTTON_DEFAULT_CONFIG();
    board_button_init(BUTTON_VOLUP_ID, &config, button_up_event_handler);
    board_button_init(BUTTON_VOLDOWN_ID, &config, button_down_event_handler); 
    board_button_init(BUTTON_PLAY_ID, &config_quick, button_power_event_handler);
#elif defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1)
    button_config_t config = BUTTON_DEFAULT_CONFIG();
    button_config_t config_quick = BUTTON_DEFAULT_CONFIG();
    config_quick.long_press_time = 600;
    board_button_init(BUTTON_VOLUP_ID, &config, button_up_event_handler);
    board_button_init(BUTTON_VOLDOWN_ID, &config, button_down_event_handler); 
    board_button_init(BUTTON_REC_ID, &config_quick, button_rec_event_handler); 
    board_button_init(BUTTON_PLAY_ID, &config, button_power_event_handler);
    board_button_init(BUTTON_MODE_ID, &config, button_mode_event_handler);
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    button_config_t config = BUTTON_DEFAULT_CONFIG();
    button_config_t config_quick = BUTTON_DEFAULT_CONFIG();
    config_quick.long_press_time = 600;
    // board_button_init(BUTTON_VOLUP_ID, &config, button_up_event_handler);
    // board_button_init(BUTTON_VOLDOWN_ID, &config, button_down_event_handler); 
    // if (BUTTON_REC_ID >= 0) {
    //     board_button_init(BUTTON_REC_ID, &config_quick, button_rec_event_handler); 
    // }
    if (BUTTON_PLAY_ID >= 0) {
        board_button_init(BUTTON_PLAY_ID, &config, button_power_event_handler);
    }
    if (BUTTON_MODE_ID >= 0) {
        board_button_init(BUTTON_MODE_ID, &config, button_mode_event_handler);
    }
#elif defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    button_config_t config = BUTTON_DEFAULT_CONFIG();
    button_config_t config_quick = BUTTON_DEFAULT_CONFIG();
    config_quick.long_press_time = 600;
    board_button_init(BUTTON_RESET_ID, &config, button_reset_event_handler);
    board_button_init(BUTTON_PLAY_ID, &config_quick, button_power_event_handler);
#endif

}
