/* Copyright 2020-2023 The TensorFlow Authors. All Rights Reserved.

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

#include <algorithm>
#include <cstdint>
#include <iterator>

#include "main_functions.h"

#include "audio_provider.h"
#include "command_responder.h"
#include "feature_provider.h"
#include "micro_model_settings.h"
#include "model.h"
#include "audio_preprocessor_int8_model_data.h"
#include "recognize_commands.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

// Globals, used for compatibility with Arduino-style sketches.
namespace {
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* model_input = nullptr;
FeatureProvider* feature_provider = nullptr;
RecognizeCommands* recognizer = nullptr;
int32_t previous_time = 0;
TaskHandle_t speech_task_handle = NULL;
bool should_run = false;
const char* TAG = "MICRO_SPEECH";

// Create an area of memory to use for input, output, and intermediate arrays.
// The size of this will depend on the model you're using, and may need to be
// determined by experimentation.
constexpr int kTensorArenaSize = 30 * 1024;
// uint8_t tensor_arena[kTensorArenaSize];
// int8_t feature_buffer[kFeatureElementCount];
uint8_t* tensor_arena = nullptr;
int8_t* feature_buffer = nullptr;
int8_t* model_input_buffer = nullptr;

// 定义回调函数类型
typedef void (*speech_recognition_callback)(const char* command, float score);
static speech_recognition_callback g_recognition_callback = nullptr;

// 添加变量用于跟踪上一次识别结果
static const char* last_command = nullptr;
static float last_score = 0.0f;
static uint32_t last_detection_time = 0;
static const uint32_t DETECTION_TIMEOUT_MS = 1000; // 1秒内需要检测到第二次
}  // namespace

static StackType_t *speech_task_stack = NULL;
static StaticTask_t speech_task_buffer;


void micro_speech_destory() {
    // 先停止任务
    if (speech_task_handle != NULL) {
        should_run = false;
        while (speech_task_handle != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // 清理 TensorFlow Lite 资源
    if (interpreter != nullptr) {
        interpreter->~MicroInterpreter();
        interpreter = nullptr;
    }

    if (feature_provider != nullptr) {
        feature_provider = nullptr;
    }

    if (recognizer != nullptr) {
        recognizer = nullptr;
    }

    if (tensor_arena != nullptr) {
        heap_caps_free(tensor_arena);
        tensor_arena = nullptr;
    }

    if (feature_buffer != nullptr) {
        heap_caps_free(feature_buffer);
        feature_buffer = nullptr;
    }

    // 重置其他指针
    model = nullptr;
    model_input = nullptr;
    model_input_buffer = nullptr;
    previous_time = 0;

    // 重置检测状态
    last_command = nullptr;
    last_score = 0.0f;
    last_detection_time = 0;

    ESP_LOGI(TAG, "Speech recognition resources destroyed");
}

// The name of this function is important for Arduino compatibility.
void micro_speech_setup(speech_recognition_callback callback) {
    // 保存回调函数
    g_recognition_callback = callback;

    // 初始化模型
    if (!model_init()) {
        ESP_LOGE(TAG, "Failed to initialize model");
        return;
    }

    if (!int8_model_init()) {
        ESP_LOGE(TAG, "Failed to initialize int8 model");
        return;
    }

    // Map the model into a usable data structure. This doesn't involve any
    // copying or parsing, it's a very lightweight operation.
    model = tflite::GetModel(g_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        MicroPrintf("Model provided is schema version %d not equal to supported "
                    "version %d.", model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    // Pull in only the operation implementations we need.
    // This relies on a complete list of all the ops needed by this graph.
    // An easier approach is to just use the AllOpsResolver, but this will
    // incur some penalty in code space for op implementations that are not
    // needed by this graph.
    //
    // tflite::AllOpsResolver resolver;
    // NOLINTNEXTLINE(runtime-global-variables)
    static tflite::MicroMutableOpResolver<4> micro_op_resolver;
    if (micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk) {
        ESP_LOGE(TAG, "AddDepthwiseConv2D failed");
        return;
    }
    if (micro_op_resolver.AddFullyConnected() != kTfLiteOk) {
        ESP_LOGE(TAG, "AddFullyConnected failed");
        return;
    }
    if (micro_op_resolver.AddSoftmax() != kTfLiteOk) {
        ESP_LOGE(TAG, "AddSoftmax failed");
        return;
    }
    if (micro_op_resolver.AddReshape() != kTfLiteOk) {
        ESP_LOGE(TAG, "AddReshape failed");
        return;
    }

    // tensor_arena 放在 PSRAM 因为空间大
    tensor_arena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    // feature_buffer 放在片内内存因为频繁访问
    feature_buffer = (int8_t*)heap_caps_malloc(kFeatureElementCount, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    if (!tensor_arena || !feature_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }

    // Build an interpreter to run the model with.
    static tflite::MicroInterpreter static_interpreter(
        model, micro_op_resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    // Allocate memory from the tensor_arena for the model's tensors.
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        MicroPrintf("AllocateTensors() failed");
        return;
    }

    // Get information about the memory area to use for the model's input.
    model_input = interpreter->input(0);
    if ((model_input->dims->size != 2) || (model_input->dims->data[0] != 1) ||
        (model_input->dims->data[1] !=
         (kFeatureCount * kFeatureSize)) ||
        (model_input->type != kTfLiteInt8)) {
        MicroPrintf("Bad input tensor parameters in model");
        return;
    }
    model_input_buffer = tflite::GetTensorData<int8_t>(model_input);

    // Prepare to access the audio spectrograms from a microphone or other source
    // that will provide the inputs to the neural network.
    // NOLINTNEXTLINE(runtime-global-variables)
    static FeatureProvider static_feature_provider(kFeatureElementCount,
                                                   feature_buffer);
    feature_provider = &static_feature_provider;

    static RecognizeCommands static_recognizer;
    recognizer = &static_recognizer;

    previous_time = 0;
}

// The name of this function is important for Arduino compatibility.
void micro_speech_loop() {
    static uint32_t last_loop_time = 0;
    uint32_t loop_start_time = esp_timer_get_time() / 1000;  // 转换为毫秒
    
    // if (last_loop_time > 0) {
    //     ESP_LOGI(TAG, "Time since last loop: %d ms", loop_start_time - last_loop_time);
    // }
    last_loop_time = loop_start_time;

    // 1. 获取音频特征
    const int32_t current_time = LatestAudioTimestamp();
    int how_many_new_slices = 0;
    
    uint32_t feature_start_time = esp_timer_get_time() / 1000;
    TfLiteStatus feature_status = feature_provider->PopulateFeatureData(
        previous_time, current_time, &how_many_new_slices);
    uint32_t feature_time = esp_timer_get_time() / 1000 - feature_start_time;
    
    // ESP_LOGI(TAG, "Feature extraction time: %d ms, new_slices: %d", feature_time, how_many_new_slices);

    if (feature_status != kTfLiteOk) {
        MicroPrintf("Feature generation failed");
        return;
    }
    previous_time = current_time;

    if (how_many_new_slices == 0) {
        return;
    }

    // 2. 复制特征数据到输入张量
    uint32_t copy_start_time = esp_timer_get_time() / 1000;
    for (int i = 0; i < kFeatureElementCount; i++) {
        model_input_buffer[i] = feature_buffer[i];
    }
    uint32_t copy_time = esp_timer_get_time() / 1000 - copy_start_time;
    // ESP_LOGI(TAG, "Feature copy time: %d ms", copy_time);

    // 3. 运行推理
    uint32_t invoke_start_time = esp_timer_get_time() / 1000;
    TfLiteStatus invoke_status = interpreter->Invoke();
    uint32_t invoke_time = esp_timer_get_time() / 1000 - invoke_start_time;
    // ESP_LOGI(TAG, "Model inference time: %d ms", invoke_time);

    if (invoke_status != kTfLiteOk) {
        MicroPrintf("Invoke failed");
        return;
    }

    // 4. 处理输出结果
    uint32_t process_start_time = esp_timer_get_time() / 1000;
    TfLiteTensor* output = interpreter->output(0);
    float output_scale = output->params.scale;
    int output_zero_point = output->params.zero_point;
    int max_idx = 0;
    float max_result = 0.0;

    for (int i = 0; i < kCategoryCount; i++) {
        float current_result =
            (tflite::GetTensorData<int8_t>(output)[i] - output_zero_point) *
            output_scale;
        if (current_result > max_result) {
            max_result = current_result;
            max_idx = i;
        }
    }
    uint32_t process_time = esp_timer_get_time() / 1000 - process_start_time;
    // ESP_LOGI(TAG, "Output processing time: %d ms", process_time);

    // 5. 总耗时统计
    uint32_t total_time = esp_timer_get_time() / 1000 - loop_start_time;
    
    if (max_result > 0.8f) {
        
        // ESP_LOGE(TAG, "result: %.2f, category: %s, total_time: %d ms", 
        //        max_result, kCategoryLabels[max_idx], total_time);
        printf("result: %.2f, category: %s, total_time: %d ms\n", 
               max_result, kCategoryLabels[max_idx], total_time);
        uint32_t current_time = esp_timer_get_time() / 1000;  // 当前时间(毫秒)

        // 如果置信度非常高(>0.95)，直接触发回调
        if (max_result > 0.95f) {
            if (g_recognition_callback) {
                g_recognition_callback(kCategoryLabels[max_idx], max_result);
            }
            // 重置状态
            last_command = nullptr;
            last_score = 0.0f;
            last_detection_time = 0;
        }
        // 否则使用两次检测策略
        else if (last_command != nullptr && 
            strcmp(last_command, kCategoryLabels[max_idx]) == 0 && 
            last_score > 0.8f &&
            (current_time - last_detection_time) < DETECTION_TIMEOUT_MS) {
            // 连续两次检测到相同命令，且时间间隔在阈值内
            if (g_recognition_callback) {
                g_recognition_callback(kCategoryLabels[max_idx], max_result);
            }
            // 重置检测状态
            last_command = nullptr;
            last_score = 0.0f;
            last_detection_time = 0;
        } else {
            // 记录本次检测结果
            last_command = kCategoryLabels[max_idx];
            last_score = max_result;
            last_detection_time = current_time;
        }
    } else {
        // 如果置信度不够，重置状态
        last_command = nullptr;
        last_score = 0.0f;
        last_detection_time = 0;
    }
}

static void speech_task(void *arg) {
    while (should_run) {
        micro_speech_loop();
        // 添加小延时让出 CPU
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    speech_task_handle = NULL;
    vTaskDelete(NULL);
}

void micro_speech_start() {
    if (speech_task_handle != NULL) {
        ESP_LOGW(TAG, "Speech task already running");
        return;
    }

    ESP_LOGI(TAG, "micro_speech_start");

    // 创建任务
    // BaseType_t ret = xTaskCreatePinnedToCore(
    //     speech_task,          // 任务函数
    //     "speech_task",        // 任务名称
    //     4 * 1024,            // 堆栈大小
    //     NULL,                // 参数
    //     5,                   // 优先级
    //     &speech_task_handle, // 任务句柄
    //     0
    // );
    // 分配堆栈内存在 PSRAM 中
    if (speech_task_stack == NULL) {
        speech_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (speech_task_stack == NULL) {
            ESP_LOGE(TAG, "Failed to allocate task stack");
            return;
        }
    }

    ESP_LOGI(TAG, "micro_speech_start");
    should_run = true;

    // 使用 xTaskCreateStatic 创建任务
    speech_task_handle = xTaskCreateStaticPinnedToCore(
        speech_task,          // 任务函数
        "speech_task",        // 任务名称
        4 * 1024,            // 堆栈大小（字节）
        NULL,                 // 参数
        3,                    // 降低优先级
        speech_task_stack,    // 堆栈缓冲区
        &speech_task_buffer,  // TCB 缓冲区
        0
    );

    ESP_LOGI(TAG, "Speech recognition started");
}

void micro_speech_stop() {
    if (speech_task_handle == NULL) {
        ESP_LOGW(TAG, "Speech task not running");
        return;
    }

    // 设置停止标志
    should_run = false;

    // 等待任务结束
    while (speech_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 清空数据但保留内存
    if (feature_buffer) {
        memset(feature_buffer, 0, kFeatureElementCount);
    }
    
    if (model_input_buffer) {
        memset(model_input_buffer, 0, kFeatureElementCount);
    }

    // 重置状态变量
    previous_time = 0;
    last_command = nullptr;
    last_score = 0.0f;
    last_detection_time = 0;

    ESP_LOGI(TAG, "Speech recognition stopped (memory retained)");
}
