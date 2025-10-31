#include "board_init.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "board.h"
#include "audio_event_iface.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "board/charge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "periph_spiffs.h"
#include "freertos/queue.h"
#include "periph_touch.h"
#include "esp_timer.h"
#include "driver/touch_pad.h"
// #include "storage/storage.h"
#include "periph_adc_button.h"
#include "input_key_service.h"
#include "board/gpio.h"
#include "storage/user_storage.h"
// #include "uart/uart_ctrl_lcd.h"
#ifdef USE_SDK_LIB
#include "sdk_dependency.h"
#else
#include "storage.h"
#endif
#include "uart_ctrl_lcd.h"
#include "battery.h"

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#include "gizwits_product.h"
#endif
#include "tt_ledc.h"
#include "audio_processor.h"

static int volume = DEFAULT_LOGIC_VOLUME;

static audio_board_handle_t audio_board_handle = NULL;
audio_board_handle_t get_audio_board_handle() {
    return audio_board_handle;
}

void set_audio_mute(bool mute) {
    if(audio_board_handle == NULL) {
        printf( "%s audio_board_handle is NULL\n", __func__);
        return;
    }
    audio_hal_set_mute(audio_board_handle->audio_hal, mute);
}

static const char* TAG = "BoardInit";

// 默认 RGB 颜色值
#define DEFAULT_RGB_RED    255
#define DEFAULT_RGB_GREEN  255
#define DEFAULT_RGB_BLUE   255
#define DEFAULT_BRIGHTNESS 50

void init_board()
{
    userInit();

// #if defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
//     init_power_hold_gpio(1);
// #endif

    tt_led_strip_init();
    battery_init();
    uint8_t power_loss_state;
    uint8_t led_state = 0;
    user_storage_load_power_loss_state(&power_loss_state, 0);
    if(power_loss_state == 1) {
        ESP_LOGI(TAG, "Power loss state %d, set to red", power_loss_state);
        TickType_t start_tick = xTaskGetTickCount();
        while(battery_get_voltage() < BATTERY_FULL_VOLTAGE) {
            ESP_LOGI(TAG, "wait for battery voltage to be 4100");
            vTaskDelay(pdMS_TO_TICKS(1000));
            led_state++;
            tt_led_strip_set_state(led_state%2?TT_LED_STATE_RED:TT_LED_STATE_OFF);
            if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(20000)) {
                ESP_LOGE(TAG, "Error: Battery voltage did not reach 4300 within 10 seconds");
                gpio_set_power_status(0); // 理论是关机了
                start_tick = xTaskGetTickCount();
            }
        }
        user_storage_save_power_loss_state(0);
        tt_led_strip_set_state(TT_LED_STATE_OFF);
    }

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    // 测试延时
    // ESP_LOGI(TAG, "Delay for testing 3s ===");
    // vTaskDelay(pdMS_TO_TICKS(3000));
    // ESP_LOGI(TAG, "Delay for testing end ===");
    // 音乐板需要初始化EX电源
    init_ext_power_en_gpio(1);
#endif

    vTaskDelay(pdMS_TO_TICKS(1000));
    audio_board_handle = audio_board_init();
    audio_hal_ctrl_codec(audio_board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    // TODO 音量从本地读取
    uint8_t volume, real_volume;
    user_storage_load_volume(&volume, DEFAULT_LOGIC_VOLUME);
    real_volume = volume * REAL_MAX_VOLUME / LOGIC_MAX_VOLUME;
    ESP_LOGI(TAG, "%s volume %d, real_volume %d", __func__, volume, real_volume);
    audio_hal_set_volume(audio_board_handle->audio_hal, real_volume);
    set_valuevolume(volume);
    set_last_valuevolume(volume);
    // set_valuevolume_delta(0);


    // au6815_i2c_master_init();
    // au6815p_reg_init();
    // au6815p_reg_play();
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t esp_set = esp_periph_set_init(&periph_cfg);


#if defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    gpio_init();

    
#ifndef CONFIG_AUDIO_BOARD_TT_MUSIC_V1
    led_strip_init();
#endif
    
    // 初始化存储系统
    esp_err_t err = user_storage_init();
    if (err == ESP_OK) {
        // 加载 RGB 颜色设置
        uint8_t r, g, b;
        user_storage_load_rgb_color(&r, &g, &b, 
                                   DEFAULT_RGB_RED, DEFAULT_RGB_GREEN, DEFAULT_RGB_BLUE);
        
        // 加载亮度设置
        uint8_t brightness;
        user_storage_load_brightness(&brightness, DEFAULT_BRIGHTNESS);
        
        // 应用 RGB 颜色和亮度设置
        ESP_LOGI(TAG, "Applying saved RGB settings: R=%d, G=%d, B=%d, Brightness=%d%%", 
                r, g, b, brightness);
        
        // 这里调用设置 RGB 颜色和亮度的函数
        // 假设有这样的函数: led_set_rgb(r, g, b, brightness);
        led_set_rgb(r, g, b);
        led_set_global_brightness(brightness);
    } else {
        ESP_LOGW(TAG, "Failed to initialize storage, using default RGB settings");
        // 使用默认设置
        led_set_rgb(DEFAULT_RGB_RED, DEFAULT_RGB_GREEN, DEFAULT_RGB_BLUE);
        led_set_global_brightness(DEFAULT_BRIGHTNESS);
    }
#endif
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1 ) || defined( CONFIG_AUDIO_BOARD_ATOM_V1_2) // CONFIG_AUDIO_BOARD_ATOM_V1_2 test
    // 初始化uart 控制眼屏
    // vTaskDelay(10 / portTICK_PERIOD_MS);
    // 上电唤醒C3自控？
    // // 发送唤醒事件
//  hall_sensor_init();
    uart_ctrl_lcd_task_init();
    // lcd_state_event_send(EVENT_INIT);

#endif
#if defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    charge_init();
#endif
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    charge_init_no_task();
#endif
    key_init(esp_set, audio_board_handle);

    led_set_state(LED_STATE_NO_CONNECT_WIFI);

#if defined(CONFIG_SERVO_SUPPORT)
    servo_init(0);
#endif

#if defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
#pragma message("CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE ENABLED")
    led_pwm_init();
    touch_pad_start();
#endif

    // 挂 spiffs
    periph_spiffs_cfg_t spiffs_cfg = {
        .root = "/spiffs",
        .partition_label = "flash_tone",
        .max_files = 99,
        .format_if_mount_failed = true};
    esp_periph_handle_t spiffs_handle = periph_spiffs_init(&spiffs_cfg);
    esp_periph_start(esp_set, spiffs_handle);
    // 启动本地播放
    audio_tone_init();
    // audio_tone_url_init();
    // audio_tone_play(1, 1, "spiffs://spiffs/converted_turn_on.mp3");


    // init_eye_lcd();

    /*
    测试代码
     */
    // send_wakeup_command();
    /*
    测试代码
     */



}




void user_set_volume_add(int vol)
{
    user_set_volume(volume + vol);
    // #if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    //     set_valuevolume_delta(vol);
    //     system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    // #endif
}

void user_set_volume_sub(int vol)
{
    user_set_volume(volume - vol);
    // #if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    //     set_valuevolume_delta(-vol);
    //     system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    // #endif
}



void user_set_volume_no_nvs(int vol) {
    ESP_LOGI(TAG, "Setting volume: %d\n", vol);
    if(vol < 0) {
        vol = 0; 
        ESP_LOGI(TAG, "Volume less than 0, set to 0\n");
    }
    if(vol > LOGIC_MAX_VOLUME) {
        vol = LOGIC_MAX_VOLUME;
        ESP_LOGI(TAG, "Volume exceeds max, set to max: %d\n", LOGIC_MAX_VOLUME);
    }
    volume = vol;
    ESP_LOGI(TAG, "Logical volume set to: %d\n", volume);

    vol = vol * REAL_MAX_VOLUME / LOGIC_MAX_VOLUME;
    
    ESP_LOGI(TAG, "Converted actual volume: %d\n", vol);
    audio_hal_set_volume(audio_board_handle->audio_hal, vol);
}

void user_set_volume(int vol) {
    ESP_LOGI(TAG, "Setting volume: %d\n", vol);
    uint8_t remainder_5 = 0;
    if(vol < 0) {
        vol = 0; 
        ESP_LOGI(TAG, "Volume less than 0, set to 0\n");
    }
    if(vol > LOGIC_MAX_VOLUME) {
        vol = LOGIC_MAX_VOLUME;
        ESP_LOGI(TAG, "Volume exceeds max, set to max: %d\n", LOGIC_MAX_VOLUME);
    }
    volume = vol;
    ESP_LOGI(TAG, "Logical volume set to: %d\n", volume);

    vol = vol * REAL_MAX_VOLUME / LOGIC_MAX_VOLUME;
    // remainder_5 = vol%5;
    // vol -= remainder_5;
    
    ESP_LOGI(TAG, "Converted actual volume: %d\n", vol);
    audio_hal_set_volume(audio_board_handle->audio_hal, vol);
    // ESP_LOGI(TAG, "Remainder: %d\n", remainder_5);
    ESP_LOGI(TAG, "waiting for 100ms: %d\n", volume);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Volume setting saving: %d\n", volume);
    user_storage_save_volume(volume);
    ESP_LOGI(TAG, "Volume setting saved: %d\n", volume);
    // #if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    //     set_valuevolume(volume);
    // #endif
}

int user_get_volume() {
    return volume;
}

void user_volume_set_tone_play() {
    set_last_audio_time(esp_timer_get_time());
    if (!get_ai_is_playing() && !get_audio_url_is_playing() && !audio_tone_url_is_playing()\
    && !get_is_playing_cache() && get_i2s_is_finished()&& get_url_i2s_is_finished()) {
        ESP_LOGI(TAG, "%s, %d", __func__, __LINE__);
        if (user_get_volume() == LOGIC_MAX_VOLUME) {
        // 提示已经最大了
            ESP_LOGI(TAG, "%s, %d", __func__, __LINE__);
            audio_tone_play(1, 0, "spiffs://spiffs/max_voice.mp3");
        } else if (user_get_volume() == LOGIC_MIN_VOLUME) {
            ESP_LOGI(TAG, "%s, %d", __func__, __LINE__);
            audio_tone_play(1, 0, "spiffs://spiffs/min_voice.mp3");
        } else {
            ESP_LOGI(TAG, "%s, %d", __func__, __LINE__);
            audio_tone_play(1, 0, "spiffs://spiffs/cur_vol_.mp3");
        }
    }
}