#include "afe_audio_processor.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr),
      consecutive_failures_(0) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // 设置AFE模块的日志级别为ERROR，忽略Ringbuffer empty警告
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    if (codec_->supports_software_aec_reference()) {
        // Force MR for software reference (single mic + reference)
        input_format = "MR";
    } else {
        for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
            input_format.push_back('M');
        }
        for (int i = 0; i < ref_num; i++) {
            input_format.push_back('R');
        }
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    // ns_model_name 暂时未使用（相关代码已注释）
    // char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    // if (ns_model_name != nullptr) {
    //     afe_config->ns_init = true;
    //     afe_config->ns_model_name = ns_model_name;
    //     afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    // } else {
    //     afe_config->ns_init = false;
    // }
    afe_config->ns_init = false;


    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 3, NULL);

    // 初始化成功，重置失败计数器
    consecutive_failures_ = 0;
    ESP_LOGI(TAG, "AFE audio processor initialized successfully");
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d, frame_samples_: %zu",
        feed_size, fetch_size, frame_samples_);

    while (true) {
        // 等待事件位被设置，如果被清除则继续等待
        EventBits_t bits = xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        
        // 检查是否应该继续运行
        if ((bits & PROCESSOR_RUNNING) == 0) {
            continue;  // 事件位被清除，继续等待
        }

        int64_t fetch_start_time = esp_timer_get_time();
        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        int64_t fetch_time = esp_timer_get_time() - fetch_start_time;
        
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            consecutive_failures_++;
            ESP_LOGW(TAG, "AFE fetch failed, consecutive failures: %d/%d, fetch耗时: %lld us", 
                     consecutive_failures_, MAX_CONSECUTIVE_FAILURES, fetch_time);
            
            // 打印任务状态信息
            TaskStatus_t task_status;
            UBaseType_t task_count = uxTaskGetNumberOfTasks();
            TaskStatus_t* task_array = (TaskStatus_t*)pvPortMalloc(task_count * sizeof(TaskStatus_t));
            if (task_array != nullptr) {
                task_count = uxTaskGetSystemState(task_array, task_count, nullptr);
                for (UBaseType_t i = 0; i < task_count; i++) {
                    if (strcmp(task_array[i].pcTaskName, "video_play") == 0 || 
                        strcmp(task_array[i].pcTaskName, "audio_communication") == 0 ||
                        strcmp(task_array[i].pcTaskName, "LVGL") == 0) {
                        ESP_LOGW(TAG, "任务状态 - %s: 优先级=%d, 运行时间=%lu, 栈剩余=%u", 
                                task_array[i].pcTaskName,
                                task_array[i].uxCurrentPriority,
                                task_array[i].ulRunTimeCounter,
                                task_array[i].usStackHighWaterMark);
                    }
                }
                vPortFree(task_array);
            }
            
            // 连续20次失败后触发异常重启
            if (consecutive_failures_ >= MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGE(TAG, "AFE audio processor fetch failed %d times consecutively, triggering abnormal restart", 
                         consecutive_failures_);
                abort();  // 触发异常，导致 ESP_RST_PANIC 重启
            }
            
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            } else {
                ESP_LOGE(TAG, "AFE fetch返回nullptr，可能是超时或资源竞争");
            }
            continue;
        }
        
        // 成功时也打印性能信息（每10次打印一次）
        // 已禁用日志打印以减少日志输出
        // static int success_count = 0;
        // success_count++;
        // if (success_count % 10 == 0) {
        //     ESP_LOGI(TAG, "AFE fetch成功，耗时: %lld us", fetch_time);
        // }
        
        // 成功读取，重置失败计数器
        consecutive_failures_ = 0;

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            ESP_LOGD(TAG, "AFE output: data_size=%d, samples=%zu, frame_samples_=%zu", 
                     res->data_size, samples, frame_samples_);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            ESP_LOGD(TAG, "AFE buffer: added %zu samples, buffer size now: %zu", samples, output_buffer_.size());
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
