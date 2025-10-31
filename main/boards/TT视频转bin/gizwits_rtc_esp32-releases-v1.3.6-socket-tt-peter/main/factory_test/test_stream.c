#include "esp_log.h"
#include "esp_check.h"
#include "audio_stream.h"
#include "filter_resample.h"
#include "raw_stream.h"
#include "spiffs_stream.h"
#include "http_stream.h"
#include "audio_common.h"
#include "driver/i2s.h"
#include "i2s_stream.h"
#include "wav_decoder.h"
#include "es7210.h"
#include "algorithm_stream.h"
#include "board.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_nsn_models.h"
#include "esp_nsn_iface.h"
#include "model_path.h"

#include "audio_stream.h"
#include "audio_processor.h"
#include "audio_pipeline.h"
#include "audio_recorder.h"
#include "audio_element.h"

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

#include "audio_stream.h"
#include "audio_processor.h"
#include "test_stream.h"
#include "audio_log.h"

static struct recorder_pipeline_t *s_ft_recorder_pipeline = NULL;
static struct player_pipeline_t   *s_ft_player_pipeline   = NULL;

static const char *TAG = "test_stream";
#define PACKAGE_MS              10
#define RECORD_BUFFER_SIZE      (16000 * 2 * PACKAGE_MS / 1000)

static char *s_record_buffer = NULL;
static int s_record_buffer_size = 0;
static volatile int s_record_buffer_index = 0;
static volatile int s_play_index = 0;

char *ft_get_record_buffer(void)
{
    return s_record_buffer;
}

int ft_get_record_buffer_index(void)
{
    return s_record_buffer_index;
}

static esp_err_t ft_i2s_read_cb(audio_element_handle_t self, char *buf, int len, TickType_t ticks_to_wait, void *context)
{
    size_t bytes_read = audio_element_input(s_ft_recorder_pipeline->i2s_stream_reader, buf, len);
    if (bytes_read > 0) {
// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
// #warning "RTC_AUDIO_SAVE_TO_FLASH enabled in ft_i2s_read_cb"
//         printf("~");
//         audio_append_to_flash_buffer(buf, bytes_read);
// #endif
    }

    return bytes_read;
}

static esp_err_t ft_recorder_write_cb(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    // 将数据写入到录音管道
    if (s_record_buffer && s_record_buffer_index + len <= s_record_buffer_size) {
        memcpy(s_record_buffer + s_record_buffer_index, buffer, len);
        s_record_buffer_index += len;
    }

    return len;
}

recorder_pipeline_handle_t ft_recorder_pipeline_open(int ch_idx)
{
    ESP_LOGI(TAG, "%s", __func__);
    recorder_pipeline_handle_t pipeline = audio_calloc(1, sizeof(struct recorder_pipeline_t));
    pipeline->record_state = PIPE_STATE_IDLE;
    s_ft_recorder_pipeline = pipeline;

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    ESP_LOGI(TAG, "es8311: set mic gain to normal");
    es8311_set_mic_gain(AUDIO_MIC_GAIN_NORMAL);
#elif defined(CONFIG_AUDIO_BOARD_ATOM_V1)
    ESP_LOGI(TAG, "es7210: set ref mic gain to 0db");
    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_0DB);
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    ESP_LOGI(TAG, "es7210: set mic gain to 30db");
    es7210_adc_set_gain(ES7210_INPUT_MIC1, GAIN_30DB);
    ESP_LOGI(TAG, "es7210: set ref mic gain to 24db");
    es7210_adc_set_gain(ES7210_INPUT_MIC2, GAIN_24DB);
#endif

    ESP_LOGI(TAG, "Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, pipeline->audio_pipeline, goto _exit_open);

    ESP_LOGI(TAG, "Create player audio stream");

    pipeline->raw_reader = create_record_raw_stream();
    AUDIO_MEM_CHECK(TAG, pipeline->raw_reader, goto _exit_open);
    // audio_element_set_write_cb(pipeline->raw_reader, ft_recorder_write_cb, NULL);

    pipeline->i2s_stream_reader = create_record_i2s_stream(false);
    AUDIO_MEM_CHECK(TAG, pipeline->i2s_stream_reader, goto _exit_open);

    pipeline->rsp_filter = create_ch2_to_ch1_48k_rsp_stream(ch_idx);
    AUDIO_MEM_CHECK(TAG, pipeline->rsp_filter, goto _exit_open);
    audio_element_set_read_cb(pipeline->rsp_filter, ft_i2s_read_cb, NULL);  

    ESP_LOGI(TAG, " Register all recorder elements to audio pipeline");
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->raw_reader, "record_raw");
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->rsp_filter, "rsp_filter");
    
    ESP_LOGI(TAG, " Link all recorder elements to audio pipeline");
    const char *link_tag[2] = {"rsp_filter", "record_raw"};
    audio_pipeline_link(pipeline->audio_pipeline, &link_tag[0], 2);

    return pipeline;

_exit_open:
    audio_pipe_safe_free(pipeline->i2s_stream_reader, audio_element_deinit);
    audio_pipe_safe_free(pipeline->rsp_filter, audio_element_deinit);
    audio_pipe_safe_free(pipeline->audio_pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(pipeline, audio_free);
    return NULL;
}

esp_err_t ft_recorder_pipeline_run(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_run(pipeline->audio_pipeline);
    pipeline->record_state = PIPE_STATE_RUNNING;
    return ESP_OK;
}

esp_err_t ft_recorder_pipeline_stop(recorder_pipeline_handle_t pipeline)
{
    ESP_RETURN_ON_FALSE(pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_stop(pipeline->audio_pipeline);
    audio_pipeline_wait_for_stop(pipeline->audio_pipeline);
    pipeline->record_state = PIPE_STATE_IDLE;
    return ESP_OK;
}

esp_err_t ft_recorder_pipeline_close(recorder_pipeline_handle_t pipeline)
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

static esp_err_t _ft_player_write_nop_cb(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    return len;
}

static esp_err_t _ft_player_i2s_write_cb(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{    
#if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
        printf("-");
        // audio_append_to_flash_buffer(buffer, len);
#endif

    return audio_element_output(s_ft_player_pipeline->i2s_stream_writer, buffer, len);
}

static esp_err_t ft_play_read_cb(audio_element_handle_t self, char *buf, int len, TickType_t ticks_to_wait, void *context)
{
    printf("*%d-%d\n", len, s_record_buffer_index);
    if (s_ft_player_pipeline && s_record_buffer && s_record_buffer_index + len <= s_record_buffer_size) {
        // 将数据写入到buf
        memcpy(buf, s_record_buffer + s_record_buffer_index, len);
        s_record_buffer_index += len;
        vTaskDelay(pdMS_TO_TICKS(PACKAGE_MS - 10));
        return len;
    } else {
        vTaskDelay(pdMS_TO_TICKS(PACKAGE_MS - 10));
        return 0;
    }
}


audio_element_handle_t ft_create_player_i2s_stream(bool enable_task)
{
    audio_element_handle_t i2s_stream = NULL;
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_1, 48000, 16, AUDIO_STREAM_WRITER);
    // i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_FMT_ONLY_LEFT);
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 16000, 16, AUDIO_STREAM_WRITER);
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_FMT_ONLY_LEFT);
#endif

    i2s_cfg.out_rb_size = 8 * 1024;
    i2s_cfg.buffer_len = 708;
    if (enable_task == false) {
        i2s_cfg.task_stack = -1;
    }
    i2s_cfg.chan_cfg.dma_desc_num = 6;
    i2s_stream = i2s_stream_init(&i2s_cfg);
    return i2s_stream;
}

player_pipeline_handle_t ft_player_pipeline_open(void)
{
    ESP_LOGI(TAG, "%s", __func__);

    if (s_ft_player_pipeline) {
        ESP_LOGI(TAG, "Player pipeline already exists");
        return s_ft_player_pipeline;
    }

    player_pipeline_handle_t player_pipeline = audio_calloc(1, sizeof(struct player_pipeline_t));
    AUDIO_MEM_CHECK(TAG, player_pipeline, goto _exit_open);
    player_pipeline->player_state = PIPE_STATE_IDLE;
    s_ft_player_pipeline = player_pipeline;

    ESP_LOGI(TAG, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    player_pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    AUDIO_MEM_CHECK(TAG, player_pipeline, goto _exit_open);

    ESP_LOGI(TAG, "Create playback audio stream");
    player_pipeline->raw_writer = create_player_raw_stream();
    AUDIO_MEM_CHECK(TAG, player_pipeline->raw_writer, goto _exit_open);
    audio_element_set_read_cb(player_pipeline->raw_writer, ft_play_read_cb, NULL);

    player_pipeline->i2s_stream_writer = ft_create_player_i2s_stream(false);
    AUDIO_MEM_CHECK(TAG, player_pipeline->i2s_stream_writer, goto _exit_open);

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    ESP_LOGI(TAG, "Create playback resample stream");
    player_pipeline->player_rsp = create_ch1_16k_to_ch2_48k_rsp_stream();
    AUDIO_MEM_CHECK(TAG, player_pipeline->player_rsp, goto _exit_open);
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->player_rsp, "player_rsp");
#endif

    ESP_LOGI(TAG, "Register all elements to playback pipeline");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->raw_writer, "player_raw");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->i2s_stream_writer, "i2s_stream_writer");
    
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    ESP_LOGI(TAG, "Link playback element together raw-->player_rsp-->i2s_stream-->[codec_chip]");
    const char *link_tag[2] = {"player_raw", "player_rsp"};
    audio_pipeline_link(player_pipeline->audio_pipeline, &link_tag[0], 2);
#else
    ESP_LOGI(TAG, "Link playback element together raw-->i2s_stream-->[codec_chip]");
    const char *link_tag[2] = {"player_raw"};
    audio_pipeline_link(player_pipeline->audio_pipeline, &link_tag[0], 1);
#endif

    audio_pipeline_run(player_pipeline->audio_pipeline);
    ESP_LOGI(TAG, "ft player pipe start running...");
    vTaskDelay(10); // 延时一小会  切换到新的 callback 请不要删除这个代码

    return player_pipeline;

_exit_open:
    s_ft_player_pipeline = NULL;
    audio_pipe_safe_free(player_pipeline->raw_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->i2s_stream_writer, audio_element_deinit);
    audio_pipe_safe_free(player_pipeline->audio_pipeline, audio_pipeline_deinit);
    audio_pipe_safe_free(player_pipeline, audio_free);
    return NULL;
}

esp_err_t ft_player_pipeline_run(player_pipeline_handle_t player_pipeline)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    if (player_pipeline->player_state == PIPE_STATE_RUNNING) {
        ESP_LOGW(TAG, "player pipe is already running state");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "ft player pipe start running 2");

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    audio_element_set_write_cb(player_pipeline->player_rsp, _ft_player_i2s_write_cb, NULL);
#else
    audio_element_set_write_cb(player_pipeline->raw_writer, _ft_player_i2s_write_cb, NULL);
#endif

    player_pipeline->player_state = PIPE_STATE_RUNNING;
    return ESP_OK;
}

esp_err_t ft_player_pipeline_stop(player_pipeline_handle_t player_pipeline)
{
    ESP_RETURN_ON_FALSE(player_pipeline != NULL, ESP_FAIL, TAG, "player pipeline not initialized");
    if (player_pipeline->player_state == PIPE_STATE_IDLE) {
        ESP_LOGW(TAG, "player pipe is idle state");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "player pipe stop running");
    
    // 停止播放
    audio_element_set_write_cb(player_pipeline->raw_writer, _ft_player_write_nop_cb, NULL);
    audio_pipeline_reset_ringbuffer(player_pipeline->audio_pipeline);
    
    player_pipeline->player_state = PIPE_STATE_IDLE;

    return ESP_OK;
}

void ft_record_task(void *arg)
{
    ft_record_arg_t *task = (ft_record_arg_t *)arg;
    int seconds = task->seconds;
    int ch_idx = task->ch_idx;

    void *buffer = s_record_buffer;
    int size = s_record_buffer_size / 2;
    int index = 0;

    ESP_LOGI(TAG, "Record task started, channel: %d, duration: %d seconds", ch_idx, seconds);

    // 检查录音管道是否已存在且正在运行
    if (s_ft_recorder_pipeline && s_ft_recorder_pipeline->record_state == PIPE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Record pipeline is already running");
        return -1;
    }

    size = s_record_buffer_size / 2;
    if (CHANNEL_AEC == ch_idx) {
        ESP_LOGW(TAG, "Start recording AEC");
        buffer = s_record_buffer;
        s_record_buffer_index = 0;
        memset(s_record_buffer, 0, s_record_buffer_size);
    } else {
        ESP_LOGW(TAG, "Start recording MIC");
        buffer = s_record_buffer + s_record_buffer_size / 2;
        s_record_buffer_index = s_record_buffer_size / 2;
        memset(buffer, 0, size);
    }

    // 打开录音管道
    if (NULL == ft_recorder_pipeline_open(ch_idx)) {
        ESP_LOGE(TAG, "Failed to open recorder pipeline");
        return -1;
    }

    // 启动录音
    if (ft_recorder_pipeline_run(s_ft_recorder_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to run recorder pipeline");
        ft_recorder_pipeline_close(s_ft_recorder_pipeline);
        return -1;
    }

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
        // 设置麦克风增益
        ESP_LOGI(TAG, "es8311: set mic gain to normal");
        es8311_set_mic_gain(AUDIO_MIC_GAIN_NORMAL);
#endif

#if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
#warning "RTC_AUDIO_SAVE_TO_FLASH enabled in ft_record_task"
        audio_log_init();
#endif

    // 循环读取录音数据
    int64_t start_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Recording start time: %lld ms", start_time);
    ESP_LOGI(TAG, "Record buffer: buf0=%p, idx=%d, rsz=%d, buf=%p, size=%d", s_record_buffer, s_record_buffer_index, RECORD_BUFFER_SIZE, buffer, size);
    while (buffer && index + RECORD_BUFFER_SIZE < size) {
        // 读取录音数据
        recorder_pipeline_read(s_ft_recorder_pipeline, buffer + index, RECORD_BUFFER_SIZE);

// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
//         printf("~");
//         audio_append_to_flash_buffer(buffer + index, RECORD_BUFFER_SIZE);
// #endif

        index += RECORD_BUFFER_SIZE;

        // 如果播放管道正在运行，则停止录音
        if (CHANNEL_AEC != ch_idx && s_ft_player_pipeline && s_ft_player_pipeline->player_state == PIPE_STATE_RUNNING) {
            ESP_LOGI(TAG, "Record task end as player started");
            break;
        }

        // 检查录音时间是否达到指定时间
        int64_t current_time = esp_timer_get_time() / 1000;
        if (current_time - start_time >= seconds * 1000) {
            ESP_LOGI(TAG, "Record task end as time out");
            break;
        }
    }

    s_record_buffer_index += index;
    ESP_LOGI(TAG, "Record task end now, index: %d", s_record_buffer_index);

// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
// #warning "RTC_AUDIO_SAVE_TO_FLASH enabled in ft_record_task"
//         audio_log_end();
// #endif

    // 停止录音
    if (ft_recorder_pipeline_stop(s_ft_recorder_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop recorder pipeline");
    }

    // 关闭录音管道
    if (ft_recorder_pipeline_close(s_ft_recorder_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to close recorder pipeline");
    }

    ESP_LOGI(TAG, "Record task completed");
    vTaskDelete(NULL);
}

void ft_init_record_buffer(int seconds)
{
    if (s_record_buffer) {
        ESP_LOGI(TAG, "Record buffer:%p already initialized, size: %d bytes", s_record_buffer, s_record_buffer_size);
        return;
    }
    
    // 计算并分配缓冲区大小
    s_record_buffer_size = seconds * 16000 * 2;
    s_record_buffer = (char *)heap_caps_malloc(s_record_buffer_size, MALLOC_CAP_SPIRAM);
    s_record_buffer_index = 0;
    s_play_index = 0;
    memset(s_record_buffer, 0, s_record_buffer_size);

    ESP_LOGI(TAG, "Record buffer:%p initialized, size: %d bytes", s_record_buffer, s_record_buffer_size);
}

int ft_start_record_task(int channel, int seconds)
{
    ft_record_arg_t arg = {
        .seconds = seconds,
        .ch_idx = channel,
    };
    // 创建录音任务
    if (xTaskGetHandle("record_task") == NULL) {
        ft_init_record_buffer(seconds*2);
        xTaskCreate((TaskFunction_t)ft_record_task, "record_task", 1024 * 4, (void *)&arg, 5, NULL);
        ESP_LOGI(TAG, "Record task created, duration: %d seconds", seconds);
        return 0;
    } else {
        ESP_LOGW(TAG, "Record task already exists");
        return -1;
    }
}

void ft_play_task(void *arg)
{
    ft_play_arg_t *task = (ft_play_arg_t *)arg;
    int seconds = task->seconds;
    void *buffer = task->buffer;
    int size = task->size;

    ESP_LOGI(TAG, "Play task started, duration: %d seconds, buffer: %p, size: %d", seconds, buffer, size);

    // 检查播放管道是否已存在且正在运行
    if (s_ft_player_pipeline && s_ft_player_pipeline->player_state == PIPE_STATE_RUNNING) {
        ESP_LOGW(TAG, "Player pipeline is already running");
        return -1;
    }

    // 打开播放管道
    if (NULL == ft_player_pipeline_open()) {
        ESP_LOGE(TAG, "Failed to open player pipeline");
        return -1;
    }

    // 启动播放
    if (ft_player_pipeline_run(s_ft_player_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to run player pipeline");
        player_pipeline_close(s_ft_player_pipeline);
        return -1;
    }

    // 循环播放录音数据
    int64_t start_time = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "Play start time: %lld ms", start_time);
    s_play_index = 0;
    ESP_LOGI(TAG, "Play buffer: buf=%p, size=%d, idx=%d", buffer, size, s_play_index);
    while (buffer && s_play_index + RECORD_BUFFER_SIZE < size) {
        printf("+");
        // printf("+%d", s_play_index);
        // 读取播放数据
        raw_stream_write(player_pipeline_get_raw_write(s_ft_player_pipeline), 
                        buffer + s_play_index, RECORD_BUFFER_SIZE);
        s_play_index += RECORD_BUFFER_SIZE;

        // 检查播放时间是否达到指定时间
        int64_t current_time = esp_timer_get_time() / 1000;
        if (current_time - start_time >= seconds * 1000) {
            ESP_LOGI(TAG, "Play task end as time out");
            break;
        }
    }
    ESP_LOGI(TAG, "Play task end now");

// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
// #warning "RTC_AUDIO_SAVE_TO_FLASH enabled in ft_play_task"
//         audio_log_end();
// #endif

    // 停止播放
    if (ft_player_pipeline_stop(s_ft_player_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop player pipeline");
    }

    // 关闭播放管道
    if (player_pipeline_close(s_ft_player_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to close player pipeline");
    }
    s_ft_player_pipeline = NULL;

    ESP_LOGI(TAG, "Play task completed");
    vTaskDelete(NULL);
}

int ft_start_play_task(int seconds, void *buffer, int size)
{
    ft_play_arg_t arg = {
        .seconds = seconds,
        .buffer = buffer,
        .size = size,
    };

    // 创建播放任务
    if (xTaskGetHandle("play_task") == NULL) { 
        xTaskCreate((TaskFunction_t)ft_play_task, "play_task", 1024 * 4, (void *)&arg, 5, NULL);
        ESP_LOGI(TAG, "Play task created, duration: %d seconds", seconds);
        return 0;
    } else {
        ESP_LOGW(TAG, "Play task already exists");
        return -1;
    }
}

