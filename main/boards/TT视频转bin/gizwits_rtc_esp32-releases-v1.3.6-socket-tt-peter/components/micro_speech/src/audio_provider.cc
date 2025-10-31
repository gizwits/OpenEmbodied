/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "audio_provider.h"

#include <cstdlib>
#include <cstring>

// FreeRTOS.h must be included before some of the following dependencies.
// Solves b/150260343.
// clang-format off
#include "freertos/FreeRTOS.h"
// clang-format on

#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "micro_model_settings.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "audio_mem.h"  // ESP-ADF memory management
#include "audio_element.h"


static const char* TAG = "TF_LITE_AUDIO_PROVIDER";
/* ringbuffer to hold the incoming audio data */
static ringbuf_handle_t g_audio_capture_buffer;
volatile int32_t g_latest_audio_timestamp = 0;
/* model requires 20ms new data from g_audio_capture_buffer and 10ms old data
 * each time , storing old data in the histrory buffer , {
 * history_samples_to_keep = 10 * 16 } */
constexpr int32_t history_samples_to_keep =
    ((kFeatureDurationMs - kFeatureStrideMs) *
     (kAudioSampleFrequency / 1000));
/* new samples to get each time from ringbuffer, { new_samples_to_get =  20 * 16
 * } */
constexpr int32_t new_samples_to_get =
    (kFeatureStrideMs * (kAudioSampleFrequency / 1000));

const int32_t kAudioCaptureBufferSize = 40000;

// static int16_t g_audio_output_buffer[kMaxAudioSampleSize * 32];
// static int16_t g_history_buffer[history_samples_to_keep];

static int16_t *g_audio_output_buffer = NULL;  // 改为指针
static int16_t *g_history_buffer = NULL;       // 改为指针

static bool g_is_audio_initialized = false;


static TaskHandle_t audio_capture_task_handle = NULL;
static bool should_capture_audio = false;

// 添加外部数据输入接口
static int16_t* external_audio_buffer = NULL;
static size_t external_audio_size = 0;

extern "C" {

// 确保采样率匹配（默认 16kHz）
// 确保数据格式匹配（16bit PCM）
// Ring buffer 大小要足够缓存数据
// 提供一个接口供外部填充数据
void provide_audio_samples(int16_t* buffer, size_t size) {
    if (!g_audio_capture_buffer) {
        ESP_LOGI(TAG, "g_audio_capture_buffer is NULL");
        return;
    }

    size_t bytes_to_write = size * sizeof(int16_t);
    size_t available_space = rb_get_size(g_audio_capture_buffer) - rb_bytes_filled(g_audio_capture_buffer);

    if (bytes_to_write > available_space) {
        uint8_t *temp_buffer = (uint8_t*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t bytes_to_drop = bytes_to_write - available_space;
        ESP_LOGW(TAG, "Ring buffer full, dropping");
        
        if (temp_buffer) {
            size_t remaining = bytes_to_drop;
            while (remaining > 0) {
                size_t chunk = (remaining > 1024) ? 1024 : remaining;
                int bytes_read = rb_read(g_audio_capture_buffer, (char*)temp_buffer, chunk, 0);
                if (bytes_read <= 0) break;
                remaining -= bytes_read;
            }
            heap_caps_free(temp_buffer);
        }
    }

    int bytes_written = rb_write(g_audio_capture_buffer,
                                (char*)buffer, 
                                bytes_to_write,
                                pdMS_TO_TICKS(100));
  
    if (bytes_written > 0) {
        // 更新时间戳
        g_latest_audio_timestamp = g_latest_audio_timestamp +
            ((1000 * (bytes_written / 2)) / kAudioSampleFrequency);
    }

    if (bytes_written != bytes_to_write) {
        ESP_LOGW(TAG, "Failed to write all data: written=%d, expected=%d",
                  bytes_written, bytes_to_write);
    }
}

// 修改 StartAudioCapture，只初始化 buffer
// TfLiteStatus StartAudioCapture() {
    
// }

// TfLiteStatus StopAudioCapture() {
//   if (audio_capture_task_handle == NULL) {
//     ESP_LOGW(TAG, "Audio capture not running");
//     return kTfLiteError;
//   }

//   should_capture_audio = false;
//   while (audio_capture_task_handle != NULL) {
//     vTaskDelay(pdMS_TO_TICKS(10));
//   }

//   if (g_audio_capture_buffer) {
//     ms_rb_cleanup(g_audio_capture_buffer);
//     g_audio_capture_buffer = NULL;
//   }

//   g_latest_audio_timestamp = 0;
//   g_is_audio_initialized = false;

//   ESP_LOGI(TAG, "Audio Recording stopped");
//   return kTfLiteOk;
// }

TfLiteStatus InitAudioRecording() {
  if (audio_capture_task_handle != NULL) {
      ESP_LOGW(TAG, "Audio capture already running");
      return kTfLiteError;
  }
  

  // g_audio_output_buffer 放在 PSRAM
  g_audio_output_buffer = (int16_t *)heap_caps_malloc(kMaxAudioSampleSize * 32 * sizeof(int16_t), 
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_audio_output_buffer) {
      ESP_LOGE(TAG, "Failed to allocate g_audio_output_buffer in PSRAM");
      return kTfLiteError;
  }

  // g_history_buffer 放在片内内存，因为访问最频繁
  g_history_buffer = (int16_t *)heap_caps_malloc(history_samples_to_keep * sizeof(int16_t), 
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!g_history_buffer) {
      ESP_LOGE(TAG, "Failed to allocate g_history_buffer in internal memory");
      heap_caps_free(g_audio_output_buffer);
      g_audio_output_buffer = NULL;
      return kTfLiteError;
  }

  g_audio_capture_buffer = rb_create(kAudioCaptureBufferSize, 1);
  if (!g_audio_capture_buffer) {
      ESP_LOGE(TAG, "Error creating ring buffer");
      return kTfLiteError;
  }

  g_is_audio_initialized = true;
  ESP_LOGI(TAG, "Audio buffer initialized");
    return kTfLiteOk;
}

TfLiteStatus StopAudioCapture() {
  if (audio_capture_task_handle == NULL) {
    ESP_LOGW(TAG, "Audio capture not running");
        return kTfLiteError;
  }

  if (g_audio_capture_buffer) {
    rb_destroy(g_audio_capture_buffer);
    g_audio_capture_buffer = NULL;
  }

  // 释放 PSRAM 内存
  if (g_audio_output_buffer) {
      heap_caps_free(g_audio_output_buffer);
      g_audio_output_buffer = NULL;
  }

  if (g_history_buffer) {
      heap_caps_free(g_history_buffer);
      g_history_buffer = NULL;
  }

  g_latest_audio_timestamp = 0;
  g_is_audio_initialized = false;

  ESP_LOGI(TAG, "Audio Recording stopped");
  return kTfLiteOk;
}

TfLiteStatus GetAudioSamples(int start_ms, int duration_ms,
                             int* audio_samples_size, int16_t** audio_samples) {
    static uint32_t last_sample_time = 0;
    uint32_t current_time = esp_timer_get_time() / 1000;
    
    // if (last_sample_time > 0) {
    //     ESP_LOGD(TAG, "Time since last audio sample: %d ms", current_time - last_sample_time);
    // }
    last_sample_time = current_time;

    if (!g_is_audio_initialized) {
        TfLiteStatus init_status = InitAudioRecording();
        if (init_status != kTfLiteOk) {
            return init_status;
        }
        g_is_audio_initialized = true;
    }
    /* copy 160 samples (320 bytes) into output_buff from history */
    memcpy((void*)(g_audio_output_buffer), (void*)(g_history_buffer),
           history_samples_to_keep * sizeof(int16_t));

    /* copy 320 samples (640 bytes) from rb at ( int16_t*(g_audio_output_buffer) +
     * 160 ), first 160 samples (320 bytes) will be from history */

    int bytes_read = rb_read(g_audio_capture_buffer,
            ((char*)(g_audio_output_buffer + history_samples_to_keep)),
            new_samples_to_get * sizeof(int16_t), pdMS_TO_TICKS(200));

    // ESP_LOGD(TAG, "Audio read: %d bytes, buffer fill: %d bytes", 
    //          bytes_read, ms_rb_filled(g_audio_capture_buffer));

    if (bytes_read < 0) {
        ESP_LOGE(TAG, " Model Could not read data from Ring Buffer");
    } else if (bytes_read < new_samples_to_get * sizeof(int16_t)) {
        ESP_LOGD(TAG, " Partial Read of Data by Model ");
        ESP_LOGV(TAG, " Could only read %d bytes when required %d bytes ",
                 bytes_read, (int) (new_samples_to_get * sizeof(int16_t)));
    }

    /* copy 320 bytes from output_buff into history */
    memcpy((void*)(g_history_buffer),
           (void*)(g_audio_output_buffer + new_samples_to_get),
           history_samples_to_keep * sizeof(int16_t));

    *audio_samples_size = kMaxAudioSampleSize;
    *audio_samples = g_audio_output_buffer;
    return kTfLiteOk;
}

int32_t LatestAudioTimestamp() { return g_latest_audio_timestamp; }

TfLiteStatus BatchGetAudioSamples(int start_ms, int slice_count,
                                 int16_t* audio_buffer, int* audio_size) {
    if (!g_is_audio_initialized) {
        TfLiteStatus init_status = InitAudioRecording();
        if (init_status != kTfLiteOk) {
            return init_status;
        }
        g_is_audio_initialized = true;
    }

    // 计算需要的总样本数
    // 每个slice需要的样本数 = (kFeatureDurationMs * kAudioSampleFrequency / 1000)
    // 重叠部分的样本数 = ((kFeatureDurationMs - kFeatureStrideMs) * kAudioSampleFrequency / 1000)
    const int samples_per_slice = kFeatureDurationMs * kAudioSampleFrequency / 1000;
    const int overlap_samples = (kFeatureDurationMs - kFeatureStrideMs) * kAudioSampleFrequency / 1000;
    const int total_samples = samples_per_slice + (slice_count - 1) * (samples_per_slice - overlap_samples);

    // ESP_LOGD(TAG, "Batch reading %d samples for %d slices", total_samples, slice_count);

    // 一次性读取所有数据
    int bytes_to_read = total_samples * sizeof(int16_t);
    int bytes_read = rb_read(g_audio_capture_buffer,
                               (char*)audio_buffer,
                               bytes_to_read,
                               pdMS_TO_TICKS(100));

    if (bytes_read != bytes_to_read) {
        ESP_LOGW(TAG, "Incomplete batch read: %d/%d bytes", bytes_read, bytes_to_read);
        return kTfLiteError;
    }

    *audio_size = total_samples;
    return kTfLiteOk;
}

} // extern "C"
