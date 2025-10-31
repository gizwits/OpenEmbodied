/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <cstring>
#include "feature_provider.h"

#include "audio_provider.h"
#include "micro_features_generator.h"
#include "micro_model_settings.h"
#include "tensorflow/lite/micro/micro_log.h"

Features g_features;
static const char *TAG = "feature_provider";

FeatureProvider::FeatureProvider(int feature_size, int8_t* feature_data)
    : feature_size_(feature_size),
      feature_data_(feature_data),
      is_first_run_(true) {
  // Initialize the feature data to default values.
  for (int n = 0; n < feature_size_; ++n) {
    feature_data_[n] = 0;
  }
}

FeatureProvider::~FeatureProvider() {}

TfLiteStatus FeatureProvider::PopulateFeatureData(
    int32_t last_time_in_ms, int32_t time_in_ms, int* how_many_new_slices) {
    int64_t total_start_time = esp_timer_get_time();  // 使用64位整数存储
    
    if (feature_size_ != kFeatureElementCount) {
        MicroPrintf("Requested feature_data_ size %d doesn't match %d",
                    feature_size_, kFeatureElementCount);
        return kTfLiteError;
    }

    const int last_step = (last_time_in_ms / kFeatureStrideMs);
    const int current_step = (time_in_ms / kFeatureStrideMs);
    int slices_needed = current_step - last_step;
    
    // 处理首次运行
    if (is_first_run_) {
        uint32_t init_start_time = esp_timer_get_time() / 1000;
        TfLiteStatus init_status = InitializeMicroFeatures();
        ESP_LOGI(TAG, "Initialize time: %d ms", esp_timer_get_time()/1000 - init_start_time);
        if (init_status != kTfLiteOk) {
            return init_status;
        }
        is_first_run_ = false;
        slices_needed = kFeatureCount;
    }

    if (slices_needed > kFeatureCount) {
        slices_needed = kFeatureCount;
    }
    *how_many_new_slices = slices_needed;

    const int slices_to_keep = kFeatureCount - slices_needed;
    const int slices_to_drop = kFeatureCount - slices_to_keep;
    // If we can avoid recalculating some slices, just move the existing data
    // up in the spectrogram, to perform something like this:
    // last time = 80ms          current time = 120ms
    // +-----------+             +-----------+
    // | data@20ms |         --> | data@60ms |
    // +-----------+       --    +-----------+
    // | data@40ms |     --  --> | data@80ms |
    // +-----------+   --  --    +-----------+
    // | data@60ms | --  --      |  <empty>  |
    // +-----------+   --        +-----------+
    // | data@80ms | --          |  <empty>  |
    // +-----------+             +-----------+
    // ESP_LOGI(TAG, "slices_needed start", slices_to_keep);
    if (slices_to_keep > 0) {
        int64_t move_start_time = esp_timer_get_time();
        for (int dest_slice = 0; dest_slice < slices_to_keep; ++dest_slice) {
            int8_t* dest_slice_data = feature_data_ + (dest_slice * kFeatureSize);
            const int src_slice = dest_slice + slices_to_drop;
            const int8_t* src_slice_data = feature_data_ + (src_slice * kFeatureSize);
            for (int i = 0; i < kFeatureSize; ++i) {
                dest_slice_data[i] = src_slice_data[i];
            }
        }
        int move_time = (esp_timer_get_time() - move_start_time) / 1000;  // 计算时间差后再转换为毫秒
        // ESP_LOGI(TAG, "Data move time: %d ms for %d slices", move_time, slices_to_keep);
    }
    // ESP_LOGI(TAG, "slices_needed: %d", slices_needed);
    // Any slices that need to be filled in with feature data have their
    // appropriate audio data pulled, and features calculated for that slice.
    if (slices_needed > 0) {
        int64_t total_start = esp_timer_get_time();
        
        // 分配批量音频缓冲区
        const int max_samples = kMaxAudioSampleSize * slices_needed;
        int16_t* batch_audio = (int16_t*)heap_caps_malloc(max_samples * sizeof(int16_t), 
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!batch_audio) {
            ESP_LOGE(TAG, "Failed to allocate batch audio buffer");
            return kTfLiteError;
        }

        // 批量读取音频
        int audio_size = 0;
        int64_t audio_start = esp_timer_get_time();
        TfLiteStatus audio_status = BatchGetAudioSamples(
            (current_step - kFeatureCount + 1) * kFeatureStrideMs,
            slices_needed, batch_audio, &audio_size);
        // ESP_LOGI(TAG, "Batch audio read time: %d ms", (int)((esp_timer_get_time() - audio_start)/1000));

        if (audio_status == kTfLiteOk) {
            // 处理每个特征窗口
            int64_t feature_start = esp_timer_get_time();
            const int stride_samples = kFeatureStrideMs * kAudioSampleFrequency / 1000;
            
            for (int new_slice = slices_to_keep; new_slice < kFeatureCount; ++new_slice) {
                int offset = (new_slice - slices_to_keep) * stride_samples;
                TfLiteStatus generate_status = GenerateFeatures(
                    batch_audio + offset, kMaxAudioSampleSize, &g_features);
                
                if (generate_status == kTfLiteOk) {
                    int8_t* new_slice_data = feature_data_ + (new_slice * kFeatureSize);
                    for (int j = 0; j < kFeatureSize; ++j) {
                        new_slice_data[j] = g_features[0][j];
                    }
                }
            }
            // ESP_LOGI(TAG, "Feature generation time: %d ms", 
            //         (int)((esp_timer_get_time() - feature_start)/1000));
        }

        heap_caps_free(batch_audio);
        // ESP_LOGI(TAG, "Total batch processing time: %d ms", 
        //          (int)((esp_timer_get_time() - total_start)/1000));
    }

    // int total_time = (esp_timer_get_time() - total_start_time) / 1000;
    // ESP_LOGI(TAG, "Total feature population time: %d ms", total_time);
    
    return kTfLiteOk;
}
