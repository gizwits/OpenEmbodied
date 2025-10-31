/* Play an MP3 file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"

#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "audio_processor.h"
#include "uart_ctrl_lcd.h"
#include "hall_switch.h"
#include "interface.h"
#include "gizwits_protocol.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif
#include "tt_ledc.h"
#include "coze_socket.h"

#define URL_PATH "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3"
#define URL_PATH2 "https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/ac386a6f804443d48067f71fe7574560.mp3~tplv-gztkv53tgq-image.image?lk3s=76f83bd8&x-expires=1778231492&x-signature=oIaTFmpagULtbJxxWpjyosw7EUw%3D"
#define URL_PATH3 "https://p3-nova-sign.byteimg.com/tos-cn-i-gztkv53tgq/ccb0bd2c40e449fa9b7ae8c3ea89d037.mp3~tplv-gztkv53tgq-image.image?lk3s=fff2e91a&x-expires=1747379609&x-signature=IHtx7aAokd7JSbWmxOWS0CaKuhs%3D"
    
static const char *TAG = "uPlay";
audio_pipeline_handle_t pipeline;
static int64_t last_pos = 0;

void print_http_elm_info(audio_element_handle_t elm);
void printf_mp3_info(audio_element_handle_t mp3_decoder, audio_element_handle_t i2s_stream_writer);

void url_mp3_play(const char *url)
{

    // 重播参数
    static uint8_t error_cnt = 0;
    int32_t start_play_time = 0;
    int32_t play_time = 0;
    uint8_t need_replay = 0;

    // 续播参数
    uint8_t seek_flag = 0;
    int64_t seek_pos = 5;
    int64_t pos = 0;
    int pos_len = 0;
    int second_recorded = 0;
    audio_element_info_t info =  { 0 };
    audio_element_info_t i2s_info =  { 0 };


restart:
    // g_recorder_pipeline_pause();
    set_voice_sleep_flag(true);
    set_manual_break_flag(true); // 这是打断当前AI讲话
    set_audio_url_player_state();
    set_is_i2s_timeout_flag(0);
    stop_audio_url_player();
    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network error_cnt:%d", error_cnt);
    // esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    // esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    // periph_wifi_cfg_t wifi_cfg = {
    //     .wifi_config.sta.ssid = "zmz667",
    //     .wifi_config.sta.password = "123456789",
    // };
    // esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    // esp_periph_start(set, wifi_handle);
    // periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    audio_element_handle_t http_stream_reader = NULL, mp3_decoder = NULL, player_rsp = NULL;
    static audio_element_handle_t i2s_stream_writer;
    // esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    // audio_board_handle_t board_handle = audio_board_init();
    // audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);


    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    // http_cfg.request_size = http_cfg.out_rb_size/2;
    // http_cfg.request_range_size = http_cfg.out_rb_size;
    // http_cfg.request_range_size = 108000;
    http_cfg.task_prio = 15;
    http_cfg.out_rb_size = 100*1024;
    ESP_LOGI(TAG, "http_stream_cfg_t details:\n"
                  "type=%d\n"
                  "out_rb_size=%d\n"
                  "task_stack=%d\n"
                  "task_core=%d\n"
                  "task_prio=%d\n"
                  "stack_in_ext=%d\n"
                  "auto_connect_next_track=%d\n"
                  "enable_playlist_parser=%d\n"
                  "request_size=%d\n"
                  "request_range_size=%d\n",
             http_cfg.type, 
             http_cfg.out_rb_size, 
             http_cfg.task_stack, 
             http_cfg.task_core, 
             http_cfg.task_prio, 
             http_cfg.stack_in_ext, 
             http_cfg.auto_connect_next_track, 
             http_cfg.enable_playlist_parser, 
             http_cfg.request_size, 
             http_cfg.request_range_size
            );
    if(http_stream_reader == NULL)
    {
        http_stream_reader = http_stream_init(&http_cfg);
    }

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    if(i2s_stream_writer == NULL) 
    {

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_1, 48000, 16, AUDIO_STREAM_WRITER);
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 16000, 16, AUDIO_STREAM_WRITER);
#endif

    i2s_cfg.task_prio = 15;
    i2s_cfg.out_rb_size = 8 * 1024;
    i2s_cfg.buffer_len = 708*3;

    i2s_cfg.chan_cfg.dma_desc_num = 6;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);


        // i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
        // i2s_cfg.type = AUDIO_STREAM_WRITER;
        // i2s_stream_writer = i2s_stream_init(&i2s_cfg);

        // i2s_stream_writer = get_player_i2s_stream();
    }
    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip %p",i2s_stream_writer);

    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
    if(mp3_decoder == NULL) 
    {
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    //    out_rb_size;       /*!< Size of output ringbuffer */
    // int                     task_stack;        /*!< Task stack size */
    // int                     task_core;         /*!< CPU core number (0 or 1) where decoder task in running */
    // int                     task_prio;    
        // mp3_cfg.out_rb_size *= 2;
        mp3_cfg.task_prio = 15;
        mp3_decoder = mp3_decoder_init(&mp3_cfg);
        // mp3_decoder = get_player_audio_decoder();
    }
    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file %p",mp3_decoder);


    // player_rsp = create_ch1_24k_to_ch2_48k_rsp_stream();
    player_rsp = create_ch1_16k_to_ch2_48k_rsp_stream();

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
    audio_pipeline_register(pipeline, player_rsp,         "player_rsp");
    audio_pipeline_register(pipeline, i2s_stream_writer,  "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[4] = {"http", "mp3","player_rsp", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 4);

    ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
    audio_element_set_uri(http_stream_reader, url);
    
    // Example of using an audio event -- START
    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    // audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    set_url_i2s_is_finished(0);

    // 延时1秒，避免颤音
    if(need_replay == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    


    audio_pipeline_run(pipeline);
    lcd_state_event_send(EVENT_REPLY);
    tt_led_strip_set_state(TT_LED_STATE_OFF);

    // TickType_t start_time = xTaskGetTickCount();

    start_play_time = esp_timer_get_time();
    


    while (1) {
        ESP_LOGI(TAG, "[ 5.1 ] Listening event from all elements of pipeline");
        lcd_state_event_send(EVENT_REPLY);


        if(seek_flag)
        {
            seek_flag = 0;
            int64_t last_play_data_len =  (play_time / 1000000 - 2) * 3* 1024; // mp3估算1秒3k byte(重播半秒)
            ESP_LOGI(TAG, "[ 5.1.1 ] Seek to %lld pos %lld, last_play_data_len%lld", seek_pos, last_pos, last_play_data_len);
            if( last_pos == 0)
            {
                pos = last_play_data_len ;
            }
            else
            {
                pos = last_pos < last_play_data_len? last_pos:last_play_data_len ;
            }
            // if(pos > 8 *1024)
            // {
            //     pos -= 8*1024;
            // }

            ESP_LOGI(TAG, "[ 5.1.2 ] Seek to %lld seek_pos %lld, last_play_data_len%lld", pos, seek_pos, last_play_data_len);


            audio_element_getinfo(mp3_decoder, &info);
            mp3_setinfo(mp3_decoder);
            audio_element_getinfo(i2s_stream_writer, &i2s_info);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            esp_err_t seek_ret = audio_element_seek(mp3_decoder, &seek_pos, sizeof(seek_pos), &pos, &pos_len);
            ESP_LOGI(TAG, "[ 5.1.3 ] Seek to %lld seek_pos %lld", pos, seek_pos);

            // 尝试不重启管道的播放
            audio_pipeline_pause(pipeline);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_pipeline_reset_ringbuffer(pipeline);
            audio_pipeline_reset_elements(pipeline);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            
            audio_element_set_byte_pos(http_stream_reader, pos);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_element_set_byte_pos(mp3_decoder, pos);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_element_getinfo(i2s_stream_writer,&i2s_info);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            // 44100 * 2 * 16 / 8
            // i2s_info.byte_pos = seek_pos * 44100 * 2 * 16 / 8;

            i2s_info.byte_pos = (int)((int64_t)seek_pos * (int64_t)info.sample_rates * (int64_t)info.channels * (int64_t)info.bits / (8 * 1));
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_element_set_byte_pos(i2s_stream_writer,info.byte_pos);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_element_set_uri(http_stream_reader, url);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            audio_pipeline_reset_items_state(pipeline);
            // // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

            audio_pipeline_resume(pipeline);
            // // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            ESP_LOGW(TAG, "[ 5.1.3 ] need_replay %d, seek_flag %d", need_replay, seek_flag);

            // set_url_i2s_is_finished(0);
            // set_audio_url_is_failed(0);
        }

        print_http_elm_info(http_stream_reader);
        // printf_mp3_info(mp3_decoder, i2s_stream_writer);



        // // todo pe 10秒后强制结束
        // if ((xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS >= 10000) {
        //     ESP_LOGW(TAG, "[ * ] 10 seconds elapsed, breaking the loop");
        //     set_i2s_is_finished(1);
        //     break;
        // }

        set_last_audio_time(esp_timer_get_time());

        // 关盖检测
        // 外部hall检测优先级有问题会不生效, 暂时在这里增加实时判断
        // int64_t hall_check_time = esp_timer_get_time();
        // while (esp_timer_get_time() - hall_check_time < 300000) {
        //     hall_timer_callback();
        // }
        
        if(get_hall_state() == HALL_STATE_OFF || get_valuestate() == state_VALUE0_close) {
            ESP_LOGI(TAG, "[ * ] Hall switch is %d, get_valuestate is %d, breaking the loop",
                get_hall_state(), get_valuestate());
            // system_os_post(USER_TASK_PRIO_2, MSG_ABORT, 0);
            set_i2s_is_abort(1);
            set_url_i2s_is_finished(0);
            break;
        }

        if(get_esp_wifi_is_connected() == 0)
        {
            // todo 
            ESP_LOGI(TAG, "[ * ] get_esp_wifi_is_connected() == 0, breaking the loop");
            system_os_post(USER_TASK_PRIO_2, MSG_FAILED, 0);
            set_i2s_is_abort(1);
            set_url_i2s_is_finished(0);
            break;
        }


        // 播放失败状态检测

        if(get_audio_url_is_failed())
        {
            play_time += esp_timer_get_time() - start_play_time; 
            ESP_LOGW(TAG, "[ 5.1 ] s_url_is_failed is %d, play_time:%d",
            get_audio_url_is_failed(), play_time);

            need_replay = 1;

            if(play_time < 3000*1000)  // 10秒内， 启播失败，直接重启管道
            {
                seek_flag = 0;
                ESP_LOGW(TAG, "[ 5.2 ] play_time%d, need_replay %d, seek_flag %d", play_time, need_replay, seek_flag);
                goto error;
            }
            else
            {
                /* cur pos get */

                audio_element_getinfo(mp3_decoder, &info);
                mp3_setinfo(mp3_decoder);
                audio_element_getinfo(i2s_stream_writer, &i2s_info);
                // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
                esp_err_t seek_ret = audio_element_seek(mp3_decoder, &seek_pos, sizeof(seek_pos), &pos, &pos_len);
                if (seek_ret == ESP_OK) {
                    ESP_LOGI(TAG, "[ * ] Seek to %lld seek_pos %lld", pos, seek_pos);
                    seek_flag = 1;
                }

                goto error;
            }
        }


        // I2S结束状态检测(包含失败,成功)
        audio_event_iface_msg_t msg;
        if(get_url_i2s_is_finished()) {
            ESP_LOGW(TAG, "[ * ] i2s_is_finished is 1");
            set_url_i2s_is_finished(0);
            // vTaskDelay(pdMS_TO_TICKS(2000));//给ADF的超时失败回调播放语音的机会

            break;

#if 0
            if(get_audio_url_is_failed() && (play_time < 10000*1000))    // 10秒内失败要重试
            {
                need_replay = 1;
                ESP_LOGW(TAG, "[ 5.1 ] play_time%d, need_replay%d",play_time, need_replay);
                goto error;
            }
            else
            {
                break;
            }
#endif

        }

        esp_err_t ret = audio_event_iface_listen(evt, &msg, 3000/portTICK_PERIOD_MS);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);

            continue;
        }
        printf("audio_event_iface_msg_t msg:\n"
                      "  cmd: %d\n"
                      "  data_len: %d\n"
                      "  source_type: %d\n"
                      "  need_free_data: %d\n", 
                      msg.cmd, 
                      msg.data_len, 
                      msg.source_type, 
                      msg.need_free_data);
        // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            // i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }
        // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
        // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    }
    // Example of using an audio event -- END
    // lcd_state_event_send(EVENT_LISTEN); // todo check？

error:
    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline %p", pipeline);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_stop(pipeline);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_wait_for_stop(pipeline);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_terminate(pipeline);

    /* Terminate the pipeline before removing the listener */
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_unregister(pipeline, http_stream_reader);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_unregister(pipeline, mp3_decoder);
    audio_pipeline_unregister(pipeline, player_rsp);

    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    // esp_periph_set_stop_all(set);
    // audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_event_iface_destroy(evt);

    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    audio_element_deinit(http_stream_reader);
    audio_element_reset_state(i2s_stream_writer);
    // audio_element_deinit(i2s_stream_writer);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // audio_element_reset_state(mp3_decoder);
    audio_element_deinit(mp3_decoder);
    audio_element_deinit(player_rsp);
    // esp_periph_set_destroy(set);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

    if(get_audio_url_is_failed() && need_replay)
    {
        need_replay = 0;
        error_cnt++;
        // if(error_cnt < 3 || seek_flag)  // 续播的直接可以重试，启播失败的可能是url有问题  # 避免无限死循环，先不要这个逻辑
        if(error_cnt < 5 )
        {
            set_audio_url_is_failed(0);
            vTaskDelay(pdMS_TO_TICKS(2000));
            audio_tone_play(2, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
            vTaskDelay(pdMS_TO_TICKS(2000));
            goto restart;
        }
        else
        {
            error_cnt = 0;
        }
    }
    else
    {
        error_cnt = 0;
    }


    reset_audio_url_player_state(0);
    // 防时序
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    set_url_i2s_is_finished(1);
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if(get_valuestate() != state_VALUE0_close) {
        lcd_state_event_send(EVENT_LISTEN);
    }
    tt_led_strip_set_state(TT_LED_STATE_OFF);
    set_manual_break_flag(false);// 这是打断当前AI讲话
    set_voice_sleep_flag(false);
    set_last_audio_time(esp_timer_get_time());

    i2s_timeout_handle();

    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // g_recorder_pipeline_resume();
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);



}
void print_http_elm_info(audio_element_handle_t elm)
{
    if (elm == NULL) {
        ESP_LOGE(TAG, "audio_element_handle_t is NULL");
        return;
    }

    audio_element_info_t info;
    esp_err_t ret = audio_element_getinfo(elm, &info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "\naudio_element info:\n"
            "  sample_rates: %d  channels: %d  bits: %d\n"
            "  bps: %d  last_pos:[ %lld ]  cur_pos:[ %lld ]  total_bytes:[ %lld ]\n"
            "  duration: %d   codec_fmt: %d\n"
            " uri: %s",
            info.sample_rates, info.channels, info.bits,
            info.bps, last_pos, info.byte_pos, info.total_bytes,
            info.duration, info.codec_fmt, info.uri ? info.uri : "NULL");

        if (info.byte_pos != last_pos) {
            last_pos = info.byte_pos;
        }
    } else {
        ESP_LOGE(TAG, "Failed to get audio element info, error code: %d", ret);
    }
}


void printf_mp3_info(audio_element_handle_t mp3_decoder, audio_element_handle_t i2s_stream_writer)
{
    int64_t seek_pos = 5;
    int64_t pos = 0;
    int pos_len = 0;
    /* cur pos get */
    audio_element_info_t info =  { 0 };
    audio_element_info_t i2s_info =  { 0 };
    audio_element_getinfo(mp3_decoder, &info);
    mp3_setinfo(mp3_decoder);
    audio_element_getinfo(i2s_stream_writer, &i2s_info);
    // ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    esp_err_t seek_ret = audio_element_seek(mp3_decoder, &seek_pos, sizeof(seek_pos), &pos, &pos_len);
    if (seek_ret == ESP_OK) {
        ESP_LOGI(TAG, "[ 5.3 ] Seek to %lld seek_pos %lld", pos, seek_pos);
    }
}