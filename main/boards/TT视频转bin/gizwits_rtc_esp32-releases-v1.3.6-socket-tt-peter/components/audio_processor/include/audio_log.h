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

#ifdef __cplusplus
extern "C" {
#endif


// #define CONFIG_RTC_AUDIO_SAVE_TO_FLASH 1
// #define CONFIG_RTC_AUDIO_SAVE_TO_SERVER 1

#ifdef CONFIG_RTC_AUDIO_SAVE_TO_FLASH
void audio_write_buffer_to_flash();
int audio_append_to_flash_buffer(char *buf, int len);
int audio_log_init();
void audio_log_end();
void audio_log_append_seperator();
#else
#define audio_write_buffer_to_flash()
#define audio_append_to_flash_buffer(buf, len)
#define audio_log_init()
#define audio_log_end()
#define audio_log_append_seperator()
#endif


#ifdef CONFIG_RTC_AUDIO_SAVE_TO_SERVER
#define AUDIO_BUFFER_SIZE       (8 * 1000)

typedef enum {
    AUDIO_BUFFER_TYPE_RAW = 0,
    AUDIO_BUFFER_TYPE_AEC = 1,
} AudioBufferType;

// 每个音频缓冲区结构
typedef struct AudioBuffer {
    struct AudioBuffer *next;  // 用链表链接不同 type 的缓冲区
    AudioBufferType type;
    size_t size;
    size_t capacity;
    uint8_t data[AUDIO_BUFFER_SIZE*2];
} AudioBuffer;

void initAudioBuffer();
void bufferAudioChunk(const uint8_t *data, size_t size, AudioBufferType type);
void uploadAudioChunk(uint8_t *data, size_t size, const char *type);
void startAudioUploadService();
#else
#define initAudioBuffer()
#define bufferAudioChunk(data, size, type)
#define uploadAudioChunk(data, size, type)
#define startAudioUploadService()
#endif


#ifdef __cplusplus
}
#endif
