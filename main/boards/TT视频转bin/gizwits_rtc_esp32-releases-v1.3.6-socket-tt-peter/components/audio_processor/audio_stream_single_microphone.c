#include "esp_log.h"

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

static const char *TAG = "audio_stream_8311";

#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
#define SAMPLE_RATE         16000
#define BIT_RATE            64000
#define COMPLEXITY          0
#define FRAME_TIME_MS       20 
#endif

#if defined (CONFIG_AUDIO_SUPPORT_AAC_DECODER)
#define SAMPLE_RATE         16000
#define BIT_RATE            80000
#endif

#if defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
#define SAMPLE_RATE         8000
#endif

#define CHANNEL             1

audio_element_handle_t create_record_encoder_stream(void)
{
    audio_element_handle_t encoder_stream = NULL;
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    raw_opus_enc_config_t opus_cfg = RAW_OPUS_ENC_CONFIG_DEFAULT();
    opus_cfg.sample_rate        = SAMPLE_RATE;
    opus_cfg.channel            = CHANNEL;
    opus_cfg.bitrate            = BIT_RATE;
    opus_cfg.complexity         = COMPLEXITY;
    opus_cfg.task_prio = 15;
    encoder_stream = raw_opus_encoder_init(&opus_cfg);
#elif defined (CONFIG_AUDIO_SUPPORT_AAC_DECODER)
    aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
    aac_cfg.sample_rate        = SAMPLE_RATE;
    aac_cfg.channel            = CHANNEL;
    aac_cfg.bitrate            = BIT_RATE;
    encoder_stream = aac_encoder_init(&aac_cfg);
#elif defined (CONFIG_AUDIO_SUPPORT_G711A_DECODER)
    g711_encoder_cfg_t g711_cfg = DEFAULT_G711_ENCODER_CONFIG();
    encoder_stream = g711_encoder_init(&g711_cfg);
#endif
    return encoder_stream;
}

audio_element_handle_t create_record_raw_stream(void)
{
    audio_element_handle_t raw_stream = NULL;
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 2 * 1024;
    raw_stream = raw_stream_init(&raw_cfg);
    audio_element_set_output_timeout(raw_stream, portMAX_DELAY);
    return raw_stream;
}


audio_element_handle_t create_algo_stream(void)

{
    ESP_LOGI(TAG, "[3.1] Create algorithm stream for aec");
    algorithm_stream_cfg_t algo_config = ALGORITHM_STREAM_CFG_DEFAULT();
    // algo_config.swap_ch = true;
    algo_config.sample_rate = 16000;
    algo_config.out_rb_size = 1024;
    algo_config.task_prio = 15;
    algo_config.algo_mask = ALGORITHM_STREAM_DEFAULT_MASK | ALGORITHM_STREAM_USE_AGC;
    audio_element_handle_t element_algo = algo_stream_init(&algo_config);
    audio_element_set_music_info(element_algo, 16000, 1, 16);
    audio_element_set_input_timeout(element_algo, portMAX_DELAY);
    return element_algo;
}

audio_element_handle_t create_record_i2s_stream(bool enable_task)
{
    audio_element_handle_t i2s_stream = NULL;
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 16000, 16, AUDIO_STREAM_READER);
    if (enable_task == false) {
        i2s_cfg.task_stack = -1;
    }
    i2s_cfg.task_prio = 15;
    i2s_stream = i2s_stream_init(&i2s_cfg);
    return i2s_stream;
}


audio_element_handle_t create_player_decoder_stream(void)
{
    audio_element_handle_t decoder_stream = NULL;
#ifdef CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    ESP_LOGI(TAG, "Create opus decoder");
    raw_opus_dec_cfg_t opus_dec_cfg = RAW_OPUS_DEC_CONFIG_DEFAULT();
    opus_dec_cfg.enable_frame_length_prefix = true;

// #if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
//     opus_dec_cfg.sample_rate = 48000;
// #else
    opus_dec_cfg.sample_rate = 16000;
// #endif

    opus_dec_cfg.channels = 1;
    opus_dec_cfg.task_core = 1;
    // opus_dec_cfg.task_prio = 13;
    decoder_stream = raw_opus_decoder_init(&opus_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_AAC_DECODER
    ESP_LOGI(TAG, "Create aac decoder");
    aac_decoder_cfg_t  aac_dec_cfg  = DEFAULT_AAC_DECODER_CONFIG();
    aac_dec_cfg.task_core = 1;
    // aac_dec_cfg.task_prio = 13;
    decoder_stream = aac_decoder_init(&aac_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_G711A_DECODER
    ESP_LOGI(TAG, "Create g711a decoder");
    g711_decoder_cfg_t g711_dec_cfg = DEFAULT_G711_DECODER_CONFIG();
    g711_dec_cfg.out_rb_size = 8 * 1024;
    g711_dec_cfg.task_core = 1;
    // g711_dec_cfg.task_prio = 13;
    decoder_stream = g711_decoder_init(&g711_dec_cfg);
#endif

    vTaskDelay(pdMS_TO_TICKS(100));
    return decoder_stream;
}

audio_element_handle_t create_player_wav_decoder_stream()
{
    audio_element_handle_t decoder_stream = NULL;
    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    decoder_stream = wav_decoder_init(&wav_cfg);
    return decoder_stream;
}

audio_element_handle_t create_player_raw_stream(void)
{
    audio_element_handle_t raw_stream = NULL;
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 8 * 1024;
    raw_stream = raw_stream_init(&raw_cfg);
    return raw_stream;
}

audio_element_handle_t create_player_i2s_stream(bool enable_task)
{
    audio_element_handle_t i2s_stream = NULL;

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_1, 48000, 16, AUDIO_STREAM_WRITER);
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 16000, 16, AUDIO_STREAM_WRITER);
#endif

    i2s_cfg.out_rb_size = 8 * 1024;
    // i2s_cfg.task_prio = 13;
    i2s_cfg.buffer_len = 708;
    if (enable_task == false) {
        i2s_cfg.task_stack = -1;
    }
    i2s_cfg.chan_cfg.dma_desc_num = 6;
    i2s_stream = i2s_stream_init(&i2s_cfg);
    return i2s_stream;
}


audio_element_handle_t create_ch1_to_ch1_8k_rsp_stream() {
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 1;
    filter_cfg.dest_rate = 8000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_ch1_to_ch2_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_ch2_16k_to_ch2_48k_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 2;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 48000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}
audio_element_handle_t create_ch1_24k_to_ch2_48k_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 24000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 48000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter_cfg.task_prio = 15;
    filter_cfg.out_rb_size *= 1.5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_ch1_16k_to_ch2_48k_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 48000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter_cfg.task_prio = 15;
    filter_cfg.out_rb_size *= 1.5;
    
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_ch2_to_ch1_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 2;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 1;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;

    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_ch2_to_ch1_48k_rsp_stream(int ch_idx)
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 2;
    filter_cfg.src_rate = 48000;
    filter_cfg.dest_ch = 1;
    filter_cfg.dest_rate = 48000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;

    filter_cfg.complexity = 0;
    // 使用down_ch_idx选择保留的源通道，假设保留第一个通道
    filter_cfg.down_ch_idx = ch_idx; // 0表示第一个通道，1表示第二个通道

    filter = rsp_filter_init(&filter_cfg);
    return filter;
}


audio_element_handle_t create_ch2_48k_to_ch2_16k_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 2;
    filter_cfg.src_rate = 48000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 1;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_16k_ch1_to_8k_ch1_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 16000;
    filter_cfg.dest_ch = 1;
    filter_cfg.dest_rate = 8000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_8k_ch1_to_16k_ch2_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 8000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_8k_ch1_to_48k_ch2_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 8000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 48000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_audio_player_spiffs_stream(void)
{
    audio_element_handle_t spiffs_stream = NULL;
    spiffs_stream_cfg_t spiffs_cfg = SPIFFS_STREAM_CFG_DEFAULT();
    spiffs_cfg.type = AUDIO_STREAM_READER;
    spiffs_cfg.task_prio = 15;
    spiffs_cfg.out_rb_size *= 2;
    spiffs_stream = spiffs_stream_init(&spiffs_cfg);
    return spiffs_stream;
}

audio_element_handle_t create_audio_player_http_stream(void)
{
    audio_element_handle_t http_stream = NULL;
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_stream = http_stream_init(&http_cfg);
    return http_stream;
}


recorder_sr_cfg_t get_default_audio_record_config(void)
{
    recorder_sr_cfg_t recorder_sr_cfg = DEFAULT_RECORDER_SR_CFG();
    recorder_sr_cfg.afe_cfg.aec_init = true;
    recorder_sr_cfg.afe_cfg.se_init = true;
    recorder_sr_cfg.multinet_init = false;
    recorder_sr_cfg.afe_cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    recorder_sr_cfg.afe_cfg.agc_mode = AFE_MN_PEAK_NO_AGC;
    recorder_sr_cfg.afe_cfg.pcm_config.mic_num = 1;
    recorder_sr_cfg.afe_cfg.pcm_config.ref_num = 1;
    recorder_sr_cfg.afe_cfg.pcm_config.total_ch_num = 2;
    recorder_sr_cfg.afe_cfg.wakenet_mode = DET_MODE_90;
    recorder_sr_cfg.input_order[0] = DAT_CH_0;
    recorder_sr_cfg.input_order[1] = DAT_CH_1;
    return recorder_sr_cfg;
}

audio_element_handle_t create_44k_ch2_to_16k_ch2_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 2;
    filter_cfg.src_rate = 44100;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}

audio_element_handle_t create_24k_ch1_to_16k_ch2_rsp_stream()
{
    audio_element_handle_t filter = NULL;
    rsp_filter_cfg_t filter_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    filter_cfg.src_ch = 1;
    filter_cfg.src_rate = 24000;
    filter_cfg.dest_ch = 2;
    filter_cfg.dest_rate = 16000;
    filter_cfg.stack_in_ext = true;
    filter_cfg.task_core = 1;
    filter_cfg.complexity = 5;
    filter = rsp_filter_init(&filter_cfg);
    return filter;
}