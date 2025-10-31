/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
 
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "sdkconfig.h"

#include "audio_element.h"
#include "raw_stream.h"
#include "recorder_sr.h"
#include "recorder_encoder.h"
#include "filter_resample.h"
#include "audio_mem.h"
#include "audio_thread.h"
#include "board.h"
#include "es7210.h"
#include "mp3_decoder.h"
#include "audio_log.h"
#include "uart_ctrl_lcd.h"

#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
#include "raw_opus_encoder.h"
#include "raw_opus_decoder.h"
#elif defined (CONFIG_AUDIO_SUPPORT_AAC_DECODER)
#include "aac_encoder.h"
#include "aac_decoder.h"
#elif defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
#include "g711_encoder.h"
#include "g711_decoder.h"
#endif
#include "audio_stream.h"
#include "audio_processor.h"
#include "gizwits_protocol.h"
#include "esp_spiffs.h"
#include "hall_switch.h"

static uint8_t s_url_is_playing = 0;
static uint8_t s_url_is_failed = 0;

static uint8_t is_i2s_timeout_flag = 0;
static int i2s_timeout_trigger_count = 0;

static const int SDK_ACTIVED_MODE_SERVER_VAD = 0;
static const int SDK_ACTIVED_MODE_BUTTON = 1;
// static const int SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP = 2;
// static const int SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP = 3;

static const char *TAG = "audio processor";

#if (defined CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1)
#define USE_ES7210_AS_RECORD_DEVICE   (0)
#define USE_ES8311_AS_RECORD_DEVICE   (1)
#elif defined CONFIG_AUDIO_BOARD_ATOM_V1 || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#define USE_ES7210_AS_RECORD_DEVICE   (1)
#define USE_ES8311_AS_RECORD_DEVICE   (0)
#else
#warning "Please according your board to config"
#endif

// aec debug
// #define ENABLE_AEC_DEBUG
#if defined (ENABLE_AEC_DEBUG)
#include <stdio.h>
#include <errno.h>
#include "esp_timer.h"

static bool    record_flag = true;
static FILE   *sfp         = NULL;
static int64_t start_tm    = 0;
#define AEC_DEBUDG_FILE_NAME "/sdcard/rec.pcm"
#define AEC_RECORD_TIME       (20)  // s
#endif  // ENABLE_AEC_DEBUG

static audio_player_t             *s_audio_player      = NULL;
static audio_player_t             *s_audio_url_player      = NULL;
static struct recorder_pipeline_t *s_recorder_pipeline = NULL;
static struct player_pipeline_t   *s_player_pipeline   = NULL;
volatile bool is_audio_tone_playing = false;

static volatile uint32_t s_i2s_last_playing_time = 0;

void set_is_i2s_timeout_flag( uint8_t flag)
{
    is_i2s_timeout_flag = flag;
}

uint32_t get_i2s_last_playing_time(void) {
    // ESP_LOGI(TAG, "get_i2s_last_playing_time: %d", s_i2s_last_playing_time);
    return s_i2s_last_playing_time;
}

void update_i2s_last_playing_time() {
    uint32_t current_time = esp_timer_get_time() / 1000;  // 转换为毫秒
    // ESP_LOGI(TAG, "set_i2s_last_playing_time: %d", current_time);
    s_i2s_last_playing_time = current_time;
}

bool get_audio_tone_playing(void) {
    return is_audio_tone_playing;
}

void set_audio_tone_playing(bool playing) {
    is_audio_tone_playing = playing;
}

#if defined (ENABLE_AEC_DEBUG)
void aec_debug_data_write(char *data, int len)
{
    if (record_flag) {
        if (sfp == NULL) {
            sfp = fopen(AEC_DEBUDG_FILE_NAME, "wb+");
            if (sfp == NULL) {
                ESP_LOGI(TAG, "Cannot open file, reason: %s\n", strerror(errno));
                return;
            }
        }
        fwrite(data, 1, len, sfp);

        if ((esp_timer_get_time() - start_tm) / 1000000 > AEC_RECORD_TIME) {
            record_flag = false;
            fclose(sfp);
            sfp = NULL;
            ESP_LOGI(TAG, "AEC debug data write done");
        }
    }
}
#endif // ENABLE_AEC_DEBUG


static esp_err_t _player_i2s_write_cb(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{    
    update_i2s_last_playing_time();
    if (should_discard_first_i2s_data()) {
        ESP_LOGW(TAG, "=== discard first i2s data: len=%d", len);
        return len;
    }
    return audio_element_output(s_player_pipeline->i2s_stream_writer, buffer, len);
}

static esp_err_t _player_write_nop_cb(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    return len;
}

static esp_err_t i2s_read_cb(audio_element_handle_t self, char *buf, int len, TickType_t ticks_to_wait, void *context)
{
    size_t bytes_read = audio_element_input(s_recorder_pipeline->i2s_stream_reader, buf, len);
    if (bytes_read > 0) {
        // printf("~");
        // printf("~%d", bytes_read);
#if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
#warning "RTC_AUDIO_SAVE_TO_FLASH enabled in i2s_read_cb"
        if (get_audio_tone_playing() || get_recording_flag()) {
            // printf("@");
            // audio_append_to_flash_buffer(buf, bytes_read);
        }
#endif
#if defined (CONFIG_RTC_AUDIO_SAVE_TO_SERVER)
        // 保存到缓冲区
        bool get_recording_flag(void);
        bool is_websocket_connected(void);
        if (get_recording_flag() && is_websocket_connected()) {
            // bufferAudioChunk((uint8_t *)buf, bytes_read, AUDIO_BUFFER_TYPE_RAW);
        }
#endif
    }

    return bytes_read;
}

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
#include "es8311.h"
#elif defined(CONFIG_AUDIO_BOARD_ATOM_V1)
#include "es7210.h"
#include "es8311.h"
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#include "es7210.h"
#include "au6815p.h"
#endif

recorder_pipeline_handle_t recorder_pipeline_open(int actived_mode)
{
    ESP_LOGI(TAG, "%s", __func__);
    recorder_pipeline_handle_t pipeline = audio_calloc(1, sizeof(struct recorder_pipeline_t));
    pipeline->record_state = PIPE_STATE_IDLE;
    s_recorder_pipeline = pipeline;

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    ESP_LOGI(TAG, "es8311: set mic gain to normal");
    es8311_set_mic_gain(AUDIO_MIC_GAIN_NORMAL);
#elif defined(CONFIG_AUDIO_BOARD_ATOM_V1)
    ESP_LOGI(TAG, "es7210: set ref mic gain to 0db");
    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_0DB);
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    ESP_LOGI(TAG, "es7210: set ref mic gain to 0db");
    es7210_adc_set_gain(ES7210_INPUT_MIC1, GAIN_12DB);// todo Peter mark mic麦克风增益
    es7210_adc_set_gain(ES7210_INPUT_MIC2, GAIN_0DB);
#endif

    ESP_LOGI(TAG, "Create audio pipeline for recording: %d", actived_mode);
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline->audio_pipeline);

    ESP_LOGI(TAG, "Create player audio stream");
    
    bool no_has_wakeup = (actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || actived_mode == SDK_ACTIVED_MODE_BUTTON);

    pipeline->raw_reader = create_record_raw_stream();

    if (no_has_wakeup) {
        pipeline->i2s_stream_reader = create_record_i2s_stream(false);
        pipeline->algo_stream = create_algo_stream();
        audio_pipeline_register(pipeline->audio_pipeline, pipeline->algo_stream, "algo");
        audio_element_set_read_cb(pipeline->algo_stream, i2s_read_cb, NULL);

        pipeline->audio_encoder = create_record_encoder_stream();
        audio_pipeline_register(pipeline->audio_pipeline, pipeline->audio_encoder, "audio_encoder");

    } else {
        pipeline->i2s_stream_reader = create_record_i2s_stream(true);
        audio_pipeline_register(pipeline->audio_pipeline, pipeline->i2s_stream_reader, "record_i2s");
    }
    
    ESP_LOGI(TAG, " Register all player elements to audio pipeline");
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->raw_reader, "record_raw");
    
    ESP_LOGI(TAG, " Link all player elements to audio pipeline");
    if (no_has_wakeup) {
        const char *link_tag[3] = {"algo", "audio_encoder", "record_raw"};
        audio_pipeline_link(pipeline->audio_pipeline, &link_tag[0], 3);
    } else {
        audio_pipeline_register(pipeline->audio_pipeline, pipeline->i2s_stream_reader, "record_i2s");
        const char *link_tag[2] = {"record_i2s", "record_raw"};
        audio_pipeline_link(pipeline->audio_pipeline, &link_tag[0], 2);
    }
    

    return pipeline;
}

esp_err_t recorder_pipeline_run(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_run(pipeline->audio_pipeline);
    pipeline->record_state = PIPE_STATE_RUNNING;
    return ESP_OK;
}

esp_err_t recorder_pipeline_stop(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_stop(pipeline->audio_pipeline);
    audio_pipeline_wait_for_stop(pipeline->audio_pipeline);
    pipeline->record_state = PIPE_STATE_IDLE;
    return ESP_OK;
}

esp_err_t recorder_pipeline_close(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_terminate(pipeline->audio_pipeline);
    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->i2s_stream_reader);
    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->raw_reader);

    /* Release all resources */
    audio_pipe_safe_free(pipeline->audio_pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(pipeline->raw_reader, audio_element_deinit);
    audio_pipe_safe_free(pipeline->i2s_stream_reader, audio_element_deinit);
    audio_pipe_safe_free(pipeline, audio_free);
    return ESP_OK;
}

int recorder_pipeline_get_default_read_size()
{
    #if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
        return 160;
    #elif defined (CONFIG_AUDIO_SUPPORT_AAC_DECODER)
        return 512;
    #elif defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
        return 160;
    #endif
    return -1;
}

audio_element_handle_t recorder_pipeline_get_raw_reader(recorder_pipeline_handle_t pipeline)
{
    return pipeline->raw_reader;
}

audio_pipeline_handle_t recorder_pipeline_get_pipeline(recorder_pipeline_handle_t pipeline)
{
    return pipeline->audio_pipeline;
}

static int input_cb_for_afe(int16_t *buffer, int buf_sz, void *user_ctx, TickType_t ticks)
{
    int size = raw_stream_read(s_recorder_pipeline->raw_reader, (char *)buffer, buf_sz);

#if defined (CONFIG_RTC_AUDIO_SAVE_TO_SERVER)
    // 保存到缓冲区
    
    bool get_recording_flag(void);
    bool is_websocket_connected(void);
    if (get_recording_flag() && is_websocket_connected()) {
        // uploadAudioChunk(buffer, buf_sz, "raw");
        bufferAudioChunk(buffer, buf_sz, AUDIO_BUFFER_TYPE_RAW);
    }
#endif

#if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
#warning "RTC_AUDIO_SAVE_TO_FLASH enabled in i2s_read_cb"
        if (get_audio_tone_playing() || get_recording_flag() || get_is_playing_cache()) {
            // printf("@");
            // audio_append_to_flash_buffer(buffer, buf_sz);
        }
#endif
    if (audio_tone_url_is_playing()) {
        printf("^");
        return 0;
    } else {
        return size;
    }
}


void * audio_record_engine_init(recorder_pipeline_handle_t pipeline, rec_event_cb_t cb, int activate_mode)
{
    recorder_sr_cfg_t recorder_sr_cfg = get_default_audio_record_config();
    recorder_encoder_cfg_t recorder_encoder_cfg = { 0 };
    recorder_encoder_cfg.encoder = create_record_encoder_stream();
    // 禁用唤醒
    if (activate_mode == SDK_ACTIVED_MODE_SERVER_VAD || activate_mode == SDK_ACTIVED_MODE_BUTTON) {
        recorder_sr_cfg.afe_cfg.vad_init = false;
        recorder_sr_cfg.afe_cfg.wakenet_init = false;
    }

    audio_rec_cfg_t cfg = AUDIO_RECORDER_DEFAULT_CFG();
    cfg.read = (recorder_data_read_t)&input_cb_for_afe;
    cfg.sr_handle = recorder_sr_create(&recorder_sr_cfg, &cfg.sr_iface);
    cfg.event_cb = cb;
    cfg.vad_off = 1000;
    cfg.wakeup_end = 120 * 1000;
    cfg.wakeup_time = 120 * 1000 * 10000; // 约等于不休眠

    if (activate_mode == SDK_ACTIVED_MODE_SERVER_VAD || activate_mode == SDK_ACTIVED_MODE_BUTTON) {
        cfg.vad_start = 0;
    }

    cfg.encoder_handle = recorder_encoder_create(&recorder_encoder_cfg, &cfg.encoder_iface);
    pipeline->recorder_engine = audio_recorder_create(&cfg);

    // 上电直接启动
    // if (activate_mode != SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP) {
    ESP_LOGI(TAG, "audio_recorder_trigger_start in %s", __FUNCTION__);
    audio_recorder_trigger_start(pipeline->recorder_engine);
    // }
    return pipeline->recorder_engine;
}

int recorder_pipeline_read(recorder_pipeline_handle_t pipeline,char *buffer, int buf_size)
{
    return raw_stream_read(pipeline->raw_reader, buffer,buf_size);
}

int audio_record_trigger_start(void *recorder_engine, int actived_mode)
{
    bool no_has_wakeup = (actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || actived_mode == SDK_ACTIVED_MODE_BUTTON);
    if (!no_has_wakeup) {
        // Use afe in wakeup mode
        ESP_LOGI(TAG, "audio_record_trigger_start");
        return audio_recorder_trigger_start(recorder_engine);
    }
    
    return 0;
}

player_pipeline_handle_t player_pipeline_open(void) 
{
    ESP_LOGI(TAG, "%s", __func__);
    player_pipeline_handle_t player_pipeline = audio_calloc(1, sizeof(struct player_pipeline_t));
    AUDIO_MEM_CHECK(TAG, player_pipeline, goto _exit_open);
    player_pipeline->player_state = PIPE_STATE_IDLE;
    s_player_pipeline = player_pipeline;

    ESP_LOGI(TAG, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    player_pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, player_pipeline, goto _exit_open);

    ESP_LOGI(TAG, "Create playback audio stream");
    player_pipeline->raw_writer = create_player_raw_stream();
    AUDIO_MEM_CHECK(TAG, player_pipeline->raw_writer, goto _exit_open);
    player_pipeline->audio_decoder = create_player_decoder_stream();
    AUDIO_MEM_CHECK(TAG, player_pipeline->audio_decoder, goto _exit_open);

    player_pipeline->i2s_stream_writer = create_player_i2s_stream(false);
    AUDIO_MEM_CHECK(TAG, player_pipeline->i2s_stream_writer, goto _exit_open);

    ESP_LOGI(TAG, "Register all elements to playback pipeline");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->raw_writer, "player_raw");
    
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->audio_decoder, "player_dec");

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    #if CONFIG_AUDIO_SUPPORT_G711A_DECODER
        player_pipeline->player_rsp = create_8k_ch1_to_48k_ch2_rsp_stream();
    #else
        player_pipeline->player_rsp = create_ch1_16k_to_ch2_48k_rsp_stream();
    #endif
#else
    #if CONFIG_AUDIO_SUPPORT_G711A_DECODER
        player_pipeline->player_rsp = create_8k_ch1_to_16k_ch2_rsp_stream();
    #else
        player_pipeline->player_rsp = create_ch1_to_ch2_rsp_stream();   
    #endif
#endif
    AUDIO_MEM_CHECK(TAG, player_pipeline->player_rsp, goto _exit_open);

    ESP_LOGI(TAG, "Link playback element together raw-->audio_decoder-->rsp-->i2s_stream-->[codec_chip]");

    vTaskDelay(pdMS_TO_TICKS(100));; // 延时一小会  切换到新的 callback 请不要删除这个代码

    ESP_LOGI(TAG, "player pipe start running...");
    audio_element_set_write_cb(player_pipeline->player_rsp, _player_write_nop_cb, NULL);
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->player_rsp, "player_rsp");
    const char *link_tag[3] = {"player_raw", "player_dec", "player_rsp"};
    audio_pipeline_link(player_pipeline->audio_pipeline, &link_tag[0], 3);

    audio_pipeline_run(player_pipeline->audio_pipeline);
    return player_pipeline;

_exit_open:
    audio_pipe_safe_free(player_pipeline->player_rsp, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->raw_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->audio_decoder, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->i2s_stream_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->audio_pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(player_pipeline, audio_free);
    return NULL;
}

esp_err_t player_pipeline_run(player_pipeline_handle_t player_pipeline)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    if (player_pipeline->player_state == PIPE_STATE_RUNNING) {
        ESP_LOGW(TAG, "player pipe is already running state");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "player pipe start running 2");
    audio_element_set_write_cb(player_pipeline->player_rsp, _player_i2s_write_cb, NULL);
    player_pipeline->player_state = PIPE_STATE_RUNNING;
    return ESP_OK;
}


esp_err_t player_pipeline_stop(player_pipeline_handle_t player_pipeline)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    if (player_pipeline->player_state == PIPE_STATE_IDLE) {
        ESP_LOGW(TAG, "player pipe is idle state");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "player pipe stop running");
    
    // 停止播放
    audio_element_set_write_cb(player_pipeline->player_rsp, _player_write_nop_cb, NULL);
    audio_pipeline_reset_ringbuffer(player_pipeline->audio_pipeline);
    
    player_pipeline->player_state = PIPE_STATE_IDLE;

    return ESP_OK;
}

esp_err_t player_pipeline_get_state(player_pipeline_handle_t player_pipeline, pipe_player_state_e *state)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    *state = player_pipeline->player_state;
    return ESP_OK;
}

esp_err_t player_pipeline_close(player_pipeline_handle_t player_pipeline)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    if (player_pipeline->player_state == PIPE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Please stop player pipe first");
        return ESP_FAIL;
    }
    audio_pipeline_terminate(player_pipeline->audio_pipeline);

    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->raw_writer);
    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->i2s_stream_writer);
    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->audio_decoder);
    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->player_rsp);

    /* Release all resources */
    audio_pipe_safe_free(player_pipeline->player_rsp, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->raw_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->i2s_stream_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->audio_decoder, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->audio_pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(player_pipeline, audio_free);
    return ESP_OK;
};

// Just create a placeholder function
int player_pipeline_get_default_read_size(player_pipeline_handle_t player_pipeline)
{
    return 0;
};

audio_element_handle_t player_pipeline_get_raw_write(player_pipeline_handle_t player_pipeline)
{
    return player_pipeline->raw_writer; 
}

static uint8_t i2s_is_finished = 1;
static uint8_t url_i2s_is_finished = 1;
static uint8_t i2s_is_abort = 0;

void __set_i2s_is_abort(const char* fun, int line, uint8_t is_abort)
{
    printf("%s %d, by %s %d\n", __func__, is_abort, fun, line);
    i2s_is_abort = is_abort;
}

uint8_t get_i2s_is_abort(void)
{
    return i2s_is_abort;
}


void __set_i2s_is_finished(uint8_t is_finished, const char *func, int line)
{
    printf("%s %d, by %s %d\n", __func__, is_finished, func, line);
    i2s_is_finished = is_finished;
}


void __set_url_i2s_is_finished(uint8_t is_finished, const char *func, int line)
{
    printf("%s %d, by %s %d\n", __func__, is_finished, func, line);
    url_i2s_is_finished = is_finished;
}


uint8_t get_url_i2s_is_finished(void)
{
    return url_i2s_is_finished;
}
uint8_t get_i2s_is_finished(void)
{
    return i2s_is_finished;
}

static void audio_player_state_task(void *arg)
{
    audio_event_iface_handle_t evt = (audio_event_iface_handle_t) arg;
    s_audio_player->running = true;
    while (s_audio_player->running) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) s_audio_player->audio_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(s_audio_player->audio_decoder, &music_info);
            set_i2s_is_finished(0);

            ESP_LOGI(TAG, "[ * ] Receive music info from wav decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }
        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) s_audio_player->i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGI(TAG, "[ * ] Stop event received");
            set_i2s_is_finished(1);
            audio_tone_stop();
            if((get_voice_sleep_flag() && !get_wakeup_flag())|| get_valuestate() == state_VALUE0_close)
            {
                ESP_LOGI(TAG, "get_voice_sleep_flag: %d, get_wakeup_flag: %d, get_valuestate: %d", get_voice_sleep_flag(), get_wakeup_flag(), get_valuestate());
                printf_cur_flag();
                lcd_state_event_send(EVENT_OFF);
            }
            else
            {
                lcd_state_event_send(EVENT_LISTEN);
            }
            is_audio_tone_playing = false;
            s_audio_player->player_state = PIPE_STATE_IDLE;
        }
    }
    ESP_LOGI(TAG, "audio_player_state_task end");
    vTaskDelete(NULL);
}



static void audio_player_url_state_task(void *arg)
{
    audio_event_iface_handle_t evt = (audio_event_iface_handle_t) arg;
    s_audio_url_player->running = true;
    while (s_audio_url_player->running) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
        ESP_LOGI(TAG, "\t msg.source_type:%d, target:%d", msg.source_type, AUDIO_ELEMENT_TYPE_ELEMENT);
        ESP_LOGI(TAG, "\t msg.source:%p, target:%p", msg.source, (void *) s_audio_url_player->audio_decoder);
        ESP_LOGI(TAG, "\t msg.cmd:%d, target:%d", msg.cmd, AEL_MSG_CMD_REPORT_MUSIC_INFO);

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) s_audio_url_player->audio_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(s_audio_url_player->audio_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from wav decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }
        
        ESP_LOGI(TAG, "\t msg.source_type:%d, target:%d", msg.source_type, AUDIO_ELEMENT_TYPE_ELEMENT);
        ESP_LOGI(TAG, "\t msg.source:%p, target:%p", msg.source, (void *) s_audio_url_player->i2s_stream_writer);
        ESP_LOGI(TAG, "\t msg.cmd:%d, target:%d", msg.cmd, AEL_MSG_CMD_REPORT_STATUS);
        ESP_LOGI(TAG, "\t msg.data:%d, target:%d", msg.data, AEL_STATUS_STATE_STOPPED);

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) s_audio_url_player->i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGI(TAG, "[ * ] Stop event received");
            s_audio_url_player->player_state = PIPE_STATE_IDLE;
        }
    }
}

audio_element_handle_t get_player_i2s_stream(void)
{
    return s_audio_player->i2s_stream_writer;
}

audio_element_handle_t get_player_audio_decoder(void)
{
    return s_audio_player->audio_decoder;
}

esp_err_t audio_tone_init(void)
{
    ESP_LOGI(TAG, "audio_tone_init");
    s_audio_player = (audio_player_t *)audio_calloc(1, sizeof(audio_player_t));
    AUDIO_MEM_CHECK(TAG, s_audio_player, goto _exit_open);

    ESP_LOGI(TAG, "Create audio pipeline for audio player");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_audio_player->pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, s_audio_player->pipeline, goto _exit_open);

    ESP_LOGI(TAG, "Create audio player audio stream");
    s_audio_player->spiffs_stream = create_audio_player_spiffs_stream();
    AUDIO_MEM_CHECK(TAG, s_audio_player->spiffs_stream, goto _exit_open);
    s_audio_player->i2s_stream_writer = create_player_i2s_stream(true);
    AUDIO_MEM_CHECK(TAG, s_audio_player->i2s_stream_writer, goto _exit_open);
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    // mp3_cfg.out_rb_size *= 2;
    // mp3_cfg.task_prio = 13;
    s_audio_player->audio_decoder = mp3_decoder_init(&mp3_cfg);
    AUDIO_MEM_CHECK(TAG, s_audio_player->audio_decoder, goto _exit_open);

    ESP_LOGI(TAG, "Register all elements to playback pipeline");
    audio_pipeline_register(s_audio_player->pipeline, s_audio_player->spiffs_stream, "audio_player_spiffs");
    audio_pipeline_register(s_audio_player->pipeline, s_audio_player->audio_decoder, "audio_player_dec");
    audio_pipeline_register(s_audio_player->pipeline, s_audio_player->i2s_stream_writer, "audio_player_i2s");

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    s_audio_player->player_rsp = create_ch2_16k_to_ch2_48k_rsp_stream();
    audio_pipeline_register(s_audio_player->pipeline, s_audio_player->player_rsp, "audio_player_rsp");

    ESP_LOGI(TAG, "Link playback element together raw-->audio_decoder-->player_rsp-->i2s_stream-->[codec_chip]");
    const char *link_tag[4] = {"audio_player_spiffs", "audio_player_dec", "audio_player_rsp", "audio_player_i2s"};
    audio_pipeline_link(s_audio_player->pipeline, &link_tag[0], 4);
#else
    ESP_LOGI(TAG, "Link playback element together raw-->audio_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"audio_player_spiffs", "audio_player_dec", "audio_player_i2s"};
    audio_pipeline_link(s_audio_player->pipeline, &link_tag[0], 3);
#endif


    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_audio_player->pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    audio_thread_create(NULL, "audio_player_state_task", audio_player_state_task, (void *)evt, 5 * 1024, 15, true, 1);

    return ESP_OK;

_exit_open:
    audio_pipe_safe_free(s_audio_player->spiffs_stream, audio_element_deinit);
    audio_pipe_safe_free(s_audio_player->i2s_stream_writer, audio_element_deinit);
    audio_pipe_safe_free(s_audio_player->audio_decoder, audio_element_deinit);
    audio_pipe_safe_free(s_audio_player->pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(s_audio_player, audio_free);
    return ESP_FAIL;
}

esp_err_t audio_tone_play_and_break_check(const char *uri)
{
    if (get_audio_url_is_playing()) {
        // 播放音乐中 停止音乐播放
        audio_tone_url_stop();
    }

    return audio_tone_play(0, 0, uri);
}

// bool file_exists(const char *file_path) {
//     struct stat st;
//     return (stat(file_path, &st) == 0);
// }

esp_err_t __audio_tone_play(uint8_t qos1, uint8_t close_can_play, const char *uri, const char* fun, int32_t line)
{
    if(qos1 == 1)// 强制播放一次
    {
        int64_t start_time = esp_timer_get_time();
        while(is_audio_tone_playing || audio_tone_url_is_playing())
        {
            if ((esp_timer_get_time() - start_time) > 2000000) { // 超过2秒
                ESP_LOGW(TAG, "play timeout, cur: %lld, start_time: %lld", esp_timer_get_time(), start_time);
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    else if(qos1 == 2)// 强制播放两次
    {
        ESP_LOGI(TAG, "%s %s force play, by [%s, %d]",__FUNCTION__, uri, fun, line);
    }
    else
    {
        if(is_audio_tone_playing || audio_tone_url_is_playing())
        {
            return ESP_FAIL;
        }
    }


    if(!close_can_play)
    {
        // 关机关盖约束
        if (get_valuestate() == state_VALUE0_close
            ||get_valuestate() == state_VALUE1_standby || get_hall_state() == HALL_STATE_OFF) {
            ESP_LOGI(TAG, "%s %s failed,by [%s, %d]",__FUNCTION__, uri, fun, line);

            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "%s %s success, by [%s, %d]",__FUNCTION__, uri, fun, line);
    // 先不加后续播的逻辑
    // while(get_is_playing_cache())
    // {
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
    
    player_pipeline_stop(s_player_pipeline);
    is_audio_tone_playing = true;
    ESP_RETURN_ON_FALSE(s_audio_player != NULL, ESP_FAIL, TAG, "audio tone not initialized");
    if (s_audio_player->player_state == PIPE_STATE_RUNNING) {
        return ESP_FAIL;
    }
    if (s_audio_player->player_state == PIPE_STATE_RUNNING) {
        audio_pipeline_stop(s_audio_player->pipeline);
        audio_pipeline_wait_for_stop(s_audio_player->pipeline);
    }
    // 将 URI 中的 "spiffs://spiffs/" 转换为 "/spiffs/"
    if (strncmp(uri, "spiffs://spiffs/", 16) == 0) {
        char file_path[256];
        snprintf(file_path, sizeof(file_path), "/spiffs/%s", uri + strlen("spiffs://spiffs/"));

        // 打开文件以检查其是否存在
        FILE* f = fopen(file_path, "r");
        if (f == NULL) {
            ESP_LOGI(TAG, "URI does not exist: %s, switching to default", uri);
            uri = "spiffs://spiffs/bo.mp3";
        } else {
            ESP_LOGI(TAG, "URI exists: %s", uri);
            fclose(f);
        }
    }

    ESP_LOGI(TAG, "audio_tone_play: %s", uri);
    audio_element_set_uri(s_audio_player->spiffs_stream, uri);
    audio_pipeline_run(s_audio_player->pipeline);
    s_audio_player->player_state = PIPE_STATE_RUNNING;
    // is_audio_tone_playing = false;
    return ESP_OK;
}

esp_err_t audio_tone_stop(void)
{
    ESP_RETURN_ON_FALSE(s_audio_player != NULL, ESP_FAIL, TAG, "audio tone not initialized");
    if (s_audio_player->player_state == PIPE_STATE_IDLE) {
        return ESP_FAIL;
    }
    audio_pipeline_stop(s_audio_player->pipeline);
    audio_pipeline_wait_for_stop(s_audio_player->pipeline);
    audio_pipeline_terminate(s_audio_player->pipeline);
    audio_pipeline_reset_ringbuffer(s_audio_player->pipeline);
    audio_pipeline_reset_elements(s_audio_player->pipeline);
    s_audio_player->player_state = PIPE_STATE_IDLE;
    return ESP_OK;
}


esp_err_t recorder_pipeline_resume(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_reset_ringbuffer(pipeline->audio_pipeline);
    audio_pipeline_reset_elements(pipeline->audio_pipeline);
    audio_pipeline_resume(pipeline->audio_pipeline);
    return ESP_OK;
};


esp_err_t recorder_pipeline_pause(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
   
    audio_pipeline_pause(pipeline->audio_pipeline);
    
    return ESP_OK;
};

esp_err_t audio_tone_url_init(void)
{
    s_audio_url_player = (audio_player_t *)audio_calloc(1, sizeof(audio_player_t));
    AUDIO_MEM_CHECK(TAG, s_audio_url_player, goto _exit_open);

    ESP_LOGI(TAG, "Create audio pipeline for audio player");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_audio_url_player->pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, s_audio_url_player->pipeline, goto _exit_open);

    ESP_LOGI(TAG, "Create audio player audio stream");
    s_audio_url_player->spiffs_stream = create_audio_player_http_stream();
    AUDIO_MEM_CHECK(TAG, s_audio_url_player->spiffs_stream, goto _exit_open);
    s_audio_url_player->i2s_stream_writer = create_player_i2s_stream(true);
    AUDIO_MEM_CHECK(TAG, s_audio_url_player->i2s_stream_writer, goto _exit_open);
    s_audio_url_player->player_rsp = create_44k_ch2_to_16k_ch2_rsp_stream();
    AUDIO_MEM_CHECK(TAG, s_audio_url_player->player_rsp, goto _exit_open);
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    // mp3_cfg.out_rb_size *= 2;
    // mp3_cfg.task_prio = 13;
    s_audio_url_player->audio_decoder = mp3_decoder_init(&mp3_cfg);
    AUDIO_MEM_CHECK(TAG, s_audio_url_player->audio_decoder, goto _exit_open);

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_audio_url_player->pipeline, evt);
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    audio_thread_create(NULL, "audio_player_url_state_task", audio_player_url_state_task, (void *)evt, 5 * 1024, 15, true, 1);


    ESP_LOGI(TAG, "Register all elements to playback pipeline");
    audio_pipeline_register(s_audio_url_player->pipeline, s_audio_url_player->spiffs_stream, "audio_player_spiffs");
    audio_pipeline_register(s_audio_url_player->pipeline, s_audio_url_player->audio_decoder, "audio_player_dec");
    audio_pipeline_register(s_audio_url_player->pipeline, s_audio_url_player->player_rsp, "audio_player_resample");
    audio_pipeline_register(s_audio_url_player->pipeline, s_audio_url_player->i2s_stream_writer, "audio_player_i2s");

    ESP_LOGI(TAG, "Link playback element together raw-->audio_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[4] = {"audio_player_spiffs", "audio_player_dec", "audio_player_resample", "audio_player_i2s"};
    audio_pipeline_link(s_audio_url_player->pipeline, &link_tag[0], 4);
    return ESP_OK;

_exit_open:
    audio_pipe_safe_free(s_audio_url_player->spiffs_stream, audio_element_deinit);
    audio_pipe_safe_free(s_audio_url_player->i2s_stream_writer, audio_element_deinit);
    audio_pipe_safe_free(s_audio_url_player->player_rsp, audio_element_deinit);
    audio_pipe_safe_free(s_audio_url_player->audio_decoder, audio_element_deinit);
    audio_pipe_safe_free(s_audio_url_player->pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(s_audio_url_player, audio_free);
    return ESP_FAIL;
}


esp_err_t audio_tone_url_play(const char *uri)
{
    player_pipeline_stop(s_audio_url_player);
    ESP_RETURN_ON_FALSE(s_audio_url_player != NULL, ESP_FAIL, TAG, "audio tone not initialized");
    if (s_audio_url_player->player_state == PIPE_STATE_RUNNING) {
        return ESP_FAIL;
    }
    if (s_audio_url_player->player_state == PIPE_STATE_RUNNING) {
        audio_pipeline_stop(s_audio_url_player->pipeline);
    }
    ESP_LOGI(TAG, "audio_tone_url_play: %s", uri);
    int ret = audio_element_set_uri(s_audio_url_player->spiffs_stream, uri);
    ESP_LOGI(TAG, "audio_element_set_uri: %d", ret);
    ret = audio_pipeline_run(s_audio_url_player->pipeline);
    ESP_LOGI(TAG, "audio_pipeline_run: %d", ret);
    s_audio_url_player->player_state = PIPE_STATE_RUNNING;
    set_audio_url_player_state();
    return ESP_OK;
}

esp_err_t audio_tone_url_stop(void)
{
    ESP_RETURN_ON_FALSE(s_audio_url_player != NULL, ESP_FAIL, TAG, "audio tone not initialized");
    if (s_audio_url_player->player_state == PIPE_STATE_IDLE) {
        return ESP_FAIL;
    }
    audio_pipeline_stop(s_audio_url_player->pipeline);
    audio_pipeline_wait_for_stop(s_audio_url_player->pipeline);
    audio_pipeline_terminate(s_audio_url_player->pipeline);
    audio_pipeline_reset_ringbuffer(s_audio_url_player->pipeline);
    audio_pipeline_reset_elements(s_audio_url_player->pipeline);
    s_audio_url_player->player_state = PIPE_STATE_IDLE;
    return ESP_OK;
}

bool get_audio_url_is_playing(void) {
    // ESP_LOGI(TAG, "audio_url_player_state: %d", s_audio_url_player->player_state == PIPE_STATE_RUNNING);
    // return s_audio_url_player->player_state == PIPE_STATE_RUNNING;
    return s_url_is_playing;
}
uint8_t get_audio_url_is_failed(void) {
    return s_url_is_failed;
}

void __set_audio_url_is_failed(const char *fun, int32_t line, bool flag)
{
    s_url_is_failed = flag;
    ESP_LOGI(TAG, "%s %d, by [%s, %d]", __func__, flag, fun, line);
}

uint8_t audio_tone_url_is_playing(void)
{
    return s_url_is_playing;
}
void set_audio_url_player_state(void)
{
    s_url_is_playing = 1;
}

void reset_audio_url_player_state(uint8_t is_failed) {
    printf("%s %d\n", __func__, is_failed);
    if(s_audio_url_player)
    {
        // s_audio_url_player->player_state = PIPE_STATE_IDLE;
    }
    // is_failed 为1 表示音频播放被中断, is_failed 为0 不改变其他事件判断的abort状态
    if(is_failed)
    {
        set_audio_url_is_failed(1);
    }

    s_url_is_playing = 0;
    set_url_i2s_is_finished(1);
}

void stop_audio_url_player(void)
{
    audio_tone_stop();
}

void stop_audio_pipeline(audio_pipeline_handle_t pipeline)
{
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_reset_ringbuffer(pipeline);
    audio_pipeline_reset_elements(pipeline);
}

void stop_recorder_pipeline()
{
    stop_audio_pipeline(s_recorder_pipeline->audio_pipeline);
}

// 间隔不超过1秒触发此回调，且，连续5秒，退出播放，连续3次，重启
#define REBOOT_COUNT 2
#define TRIGGER_TIME 12000
#define LAST_MIN_TIME 1000


// 测试功能使用（每次http播放url i2s前300ms也会触发timeout回调）
// #define REBOOT_COUNT 2
// #define TRIGGER_TIME 100
// #define LAST_MIN_TIME 1000

void i2s_timeout_check_callback(void) {

    static TickType_t first_entry_time = 0;
    static TickType_t last_entry_time = 0;

    printf("i2s_to %d ft %d lt%d\n", i2s_timeout_trigger_count, first_entry_time, last_entry_time);

    TickType_t current_time = xTaskGetTickCount();

    // 如果是第一次进入，记录第一次进入时间
    if (first_entry_time == 0) {
        first_entry_time = current_time;
        last_entry_time = current_time;
    }

    // 检查是否在1秒内进入
    if (current_time - last_entry_time <= pdMS_TO_TICKS(LAST_MIN_TIME)) {
        // 刷新最后一次进入时间
        last_entry_time = current_time;
    } else {
        // 超过1秒未进入，重置第一次进入时间
        first_entry_time = current_time;
        last_entry_time = current_time;
        i2s_timeout_trigger_count = 0;
    }

    // 如果10秒内每秒都进入
    if (last_entry_time - first_entry_time >= pdMS_TO_TICKS(TRIGGER_TIME)) {
        i2s_timeout_trigger_count++;
        // 重置第一次进入时间
        first_entry_time = 0;
        last_entry_time = 0;
        set_audio_url_is_failed(1);
        // reset_audio_url_player_state(1);

        is_i2s_timeout_flag = 1;
    }
}

void i2s_timeout_handle(void)
{
    if(!is_i2s_timeout_flag)
    {
        return;
    }
    printf("%s %d cnt: %d\n", __func__, __LINE__, i2s_timeout_trigger_count);
    vTaskDelay(pdMS_TO_TICKS(2000));
    audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    if(i2s_timeout_trigger_count > 2)
    {
        esp_restart(); // 调用ESP32的重启函数
    }

    is_i2s_timeout_flag = 0;
}

