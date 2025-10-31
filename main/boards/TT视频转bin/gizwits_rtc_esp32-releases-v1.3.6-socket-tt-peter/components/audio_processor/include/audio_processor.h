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


#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "audio_pipeline.h"
#include "audio_recorder.h"

#include "audio_log.h"
#include "es8311.h"

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1)
#define AUDIO_MIC_GAIN_NORMAL           ES8311_MIC_GAIN_24DB
#define AUDIO_MIC_GAIN_LESS_SENSITIVE   ES8311_MIC_GAIN_24DB
#elif defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
#define AUDIO_MIC_GAIN_NORMAL           ES8311_MIC_GAIN_30DB
#define AUDIO_MIC_GAIN_LESS_SENSITIVE   ES8311_MIC_GAIN_30DB
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#define AUDIO_MIC_GAIN_NORMAL           GAIN_30DB
#define AUDIO_MIC_GAIN_LESS_SENSITIVE   GAIN_3DB
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enumeration of player pipeline states
 */
typedef enum {
    PIPE_STATE_IDLE,     /**< The pipeline is idle and not processing any audio */
    PIPE_STATE_RUNNING,  /**< The pipeline is actively processing and playing audio */
} pipe_player_state_e;


#define audio_pipe_safe_free(x, fn) do { \
    if (x) {                             \
        fn(x);                           \
        x = NULL;                        \
    }                                    \
} while (0)

struct player_pipeline_t {
    audio_pipeline_handle_t audio_pipeline;
    audio_element_handle_t  raw_writer;
    audio_element_handle_t  audio_decoder;
    audio_element_handle_t  i2s_stream_writer;
    audio_element_handle_t  player_rsp;
    pipe_player_state_e     player_state;
};

typedef struct {
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t  spiffs_stream;
    audio_element_handle_t  player_rsp;
    audio_element_handle_t  audio_decoder; // only support for wav; 16k, 16bit, double channel
    audio_element_handle_t  i2s_stream_writer;
    pipe_player_state_e     player_state;
    bool                    running;
} audio_player_t;

struct recorder_pipeline_t {
    audio_pipeline_handle_t audio_pipeline;
    audio_element_handle_t  i2s_stream_reader;
    audio_element_handle_t  audio_encoder;
    audio_element_handle_t  rsp_filter;
    audio_element_handle_t  raw_reader;
    ringbuf_handle_t       raw_ringbuf;
    audio_element_handle_t  algo_stream;
    audio_rec_handle_t     recorder_engine;
    pipe_player_state_e    record_state;
};

/**
 * @brief  A handle to the recorder pipeline structure
 */
typedef struct recorder_pipeline_t* recorder_pipeline_handle_t;

/**
 * @brief  A handle to the player pipeline structure
 */
typedef struct player_pipeline_t *player_pipeline_handle_t;

/**
 * @brief  Opens and initializes the recorder pipeline
 *
 * This function sets up the necessary resources and configurations for the recorder pipeline
 *
 * @return 
 *       - A handle to the initialized recorder pipeline
 *       - NULL if the operation fails
 */
recorder_pipeline_handle_t recorder_pipeline_open(int actived_mode);

/**
 * @brief  Starts the recorder pipeline
 *
 * This function begins recording audio using the specified pipeline. It must be called after `recorder_pipeline_open()
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 * 
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_run(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief  Stops the recorder pipeline.
 *
 * This function halts the audio recording and ensures that all resources used by the pipeline are properly released
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_stop(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief Closes the recorder pipeline and releases resources.
 *
 * This function cleans up all resources associated with the recorder pipeline. It should be called after `recorder_pipeline_stop()
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t recorder_pipeline_close(recorder_pipeline_handle_t recorder_pipeline);

/**
 * @brief  Gets the default buffer size for reading data from the recorder pipeline
 *
 * This function returns the recommended buffer size for optimal data reading from the recorder pipeline
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline
 *
 * @return
 *       - > 0                 The specific length of data being read
 *       - <= 0                Error code
 */
int recorder_pipeline_get_default_read_size();

/**
 * @brief  Reads audio data from the recorder pipeline
 *
 * @param[in]  recorder_pipeline  The handle to the recorder pipeline.
 * @param[in]  buffer             The buffer to store the audio data.
 * @param[in]  buf_size           The size of the buffer in bytes.
 *
 * @return
 *       - > 0                 The specific length of data being read
 *       - <= 0                Error code
 */
int recorder_pipeline_read(recorder_pipeline_handle_t recorder_pipeline, char *buffer, int buf_size);

/**
 * @brief Reads audio data from the recorder engine
 *
 * This function reads audio data from the recorder engine and stores it in the provided buffer
 *
 * @param[in]  recorder_engine  The handle to the recorder engine
 * @param[in]  buffer           The buffer to store the audio data
 * @param[in]  buf_size         The size of the buffer in bytes
 *
 * @return
 *       - > 0                 The specific length of data being read
 *       - <= 0                Error code
 */
int audio_recorder_read(void *recorder_engine, char *buffer, int buf_size);

/**
 * @brief Starts the recording trigger
 *
 * This function starts the recording trigger for the given recorder engine
 *
 * @param[in]  recorder_engine  The handle to the recorder engine
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
int audio_record_trigger_start(void *recorder_engine, int actived_mode);

/**
 * @brief Opens and initializes the player pipeline
 *
 * @return A handle to the initialized player pipeline, or NULL if the operation fails
 */
player_pipeline_handle_t player_pipeline_open(void);

/**
 * @brief  Starts the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_run(player_pipeline_handle_t player_pipeline);

/**
 * @brief Closes the player pipeline and releases resources
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_close(player_pipeline_handle_t player_pipeline);

/**
 * @brief  Stop the player pipeline
 *
 * @param[in]  player_pipeline   handle to the player pipeline instance
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_stop(player_pipeline_handle_t player_pipeline);

/**
 * @brief  Get the current state of the player pipeline
 *
 * @param[in]   player_pipeline  The handle to the player pipeline instance
 * @param[out]  state            A pointer to a variable where the pipeline state will be stored
 *                               The state will be set to one of the values defined in the 
 *                               `pipe_player_state_e` enumeration
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t player_pipeline_get_state(player_pipeline_handle_t player_pipeline, pipe_player_state_e *state);

/**
 * @brief Gets the default buffer size for writing data to the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return 
 *       - The default buffer size in bytes
 */
int player_pipeline_get_default_read_size(player_pipeline_handle_t player_pipeline);

/**
 * @brief Writes audio data to the player pipeline
 *
 * This function sends audio data to the pipeline for playback
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 * @param[in]  buffer           The buffer containing the audio data to be written
 * @param[in]  buf_size         The size of the audio data in bytes
 *
 * @return 
 *        - >= 0  The number of bytes successfully written
 *        - < 0   If an error occurs
 */
int player_pipeline_write(player_pipeline_handle_t player_pipeline, char *buffer, int buf_size);

/**
 * @brief Gets the raw write element handle from the player pipeline
 *
 * @param[in]  player_pipeline  The handle to the player pipeline
 *
 * @return 
        - The raw write element handle
        -  NULL, if the operation fails
 */
audio_element_handle_t player_pipeline_get_raw_write(player_pipeline_handle_t player_pipeline);

/**
 * @brief Initializes the audio recording engine with the specified pipeline and callback
 *
 * @param[in]  pipeline  The handle to the recorder pipeline to be used by the recording engine
 * @param[in]  cb        The callback function to handle recording events
 *
 * @return A pointer to the initialized audio recording engine instance, or NULL if the initialization fails.
 *         The returned pointer must be managed appropriately, including cleanup when no longer needed.
 */
void *audio_record_engine_init(recorder_pipeline_handle_t pipeline, rec_event_cb_t cb, int activate_mode);

/**
 * @brief Initialize the audio tone.
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t audio_tone_init(void);

/**
 * @brief  Play audio tone from a specified URI
 *
 * @param[in]  uri  The URI of the audio source to play. This can be a file path 
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
#
esp_err_t __audio_tone_play(uint8_t qos1, uint8_t close_can_play, const char *uri, const char* fun, int32_t line);
#define audio_tone_play(qos1, force, uri) __audio_tone_play(qos1, force, uri, __func__, __LINE__)

/**
 * @brief Stop audio tone playback
 *
 * @return
 *      - ESP_OK   if the operation is successful
 *      - ESP_FAIL if the operation fails
 */
esp_err_t audio_tone_stop(void);

bool get_audio_tone_playing(void);

void set_audio_tone_playing(bool playing);

esp_err_t audio_tone_play_and_break_check(const char *uri);
bool get_audio_url_is_playing(void);
uint8_t audio_tone_url_is_playing(void);
audio_element_handle_t get_player_i2s_stream(void);
void __set_i2s_is_finished(uint8_t is_finished, const char *func, int line);
#define set_i2s_is_finished(is_finished) __set_i2s_is_finished(is_finished, __func__, __LINE__)


void __set_url_i2s_is_finished(uint8_t is_finished, const char *func, int line);
#define set_url_i2s_is_finished(is_finished) __set_url_i2s_is_finished(is_finished, __func__, __LINE__)

uint8_t get_i2s_is_finished(void);
audio_element_handle_t get_player_audio_decoder(void);
void stop_audio_url_player(void);

bool get_audio_tone_playing(void);

void set_audio_tone_playing(bool playing);

esp_err_t audio_tone_play_and_break_check(const char *uri);
bool get_audio_url_is_playing(void);
void stop_recorder_pipeline(void);
void audio_processor_init(void);

void __set_audio_url_is_failed(const char *fun, int32_t line, bool flag);
#define set_audio_url_is_failed(flag) __set_audio_url_is_failed(__func__, __LINE__, flag)
#define set_i2s_is_abort(is_abort) __set_i2s_is_abort(__func__, __LINE__, is_abort)
void __set_i2s_is_abort(const char* fun, int line, uint8_t is_abort);

#ifdef __cplusplus
}
#endif
