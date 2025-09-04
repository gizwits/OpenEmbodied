#include "audio_service.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "wake_words/afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "wake_words/esp_wake_word.h"
#elif CONFIG_USE_CUSTOM_WAKE_WORD
#include "wake_words/custom_wake_word.h"
#endif

#define TAG "AudioService"


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    
#ifndef CONFIG_USE_EYE_STYLE_VB6824
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);
#endif

#ifndef CONFIG_USE_EYE_STYLE_VB6824
    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#elif CONFIG_USE_CUSTOM_WAKE_WORD
    wake_word_ = std::make_unique<CustomWakeWord>();
#else
    wake_word_ = nullptr;
#endif

#ifdef CONFIG_USE_EYE_STYLE_VB6824
    audio_processor_->OnOutput([this](std::vector<uint8_t>&& opus) {
        ESP_LOGD(TAG, "Audio processor output: opus.size()=%u", (unsigned int)opus.size());
        // 直接推送到管道
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->payload = std::move(opus);
        packet->sample_rate = 16000;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        packet->timestamp = 0;
        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            audio_send_queue_.push_back(std::move(packet));
        }
        if (callbacks_.on_send_queue_available) {
            callbacks_.on_send_queue_available();
        }
    });
#else
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        // ESP_LOGW(TAG, "Audio processor output: data.size()=%u", (unsigned int)data.size());
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
#endif


    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }

    // 内存优化：预分配音频缓冲区减少内存碎片
    if (!buffers_initialized_) {
        // 预分配解码缓冲区：60ms @ 16kHz = 960 samples
        size_t decode_buffer_size = OPUS_FRAME_DURATION_MS * 16000 / 1000;
        decode_pcm_buffer_.reserve(decode_buffer_size);
#ifndef CONFIG_USE_EYE_STYLE_VB6824
        resample_buffer_.reserve(decode_buffer_size * 2);
#endif
        buffers_initialized_ = true;
    }

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 1);

    /* Start the audio output task */
    // xTaskCreate([](void* arg) {
    //     AudioService* audio_service = (AudioService*)arg;
    //     audio_service->AudioOutputTask();
    //     vTaskDelete(NULL);
    // }, "audio_output", 2048 * 2, this, 3, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    int input_task_size = 1024 *4;
#ifdef CONFIG_IDF_TARGET_ESP32C2
    input_task_size = 1024 * 2;
#endif
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", input_task_size, this, 8, &audio_input_task_handle_);  // 提高优先级从 8 到 6

    /* Start the audio output task */
    // xTaskCreate([](void* arg) {
    //     AudioService* audio_service = (AudioService*)arg;
    //     audio_service->AudioOutputTask();
    //     vTaskDelete(NULL);
    // }, "audio_output", 2048 + 768, this, 3, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    int task_size = 2048 * 13;
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    task_size = 1024 * 8;  // 减少栈大小，因为不需要编码逻辑
#endif
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", task_size, this, 2, &opus_codec_task_handle_);  // 提高优先级从 2 到 5
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
#ifndef CONFIG_USE_EYE_STYLE_VB6824
    audio_encode_queue_.clear();
#endif
    audio_decode_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

#ifndef CONFIG_USE_EYE_STYLE_VB6824
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        codec_->EnableInput(true);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate);
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}
#else
// opus 编码
bool AudioService::ReadAudioData(std::vector<uint8_t>& opus, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        codec_->EnableInput(true);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    }
    opus.resize(samples);
    if (!codec_->InputData(opus)) {
        return false;
    }
    
    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;
    
    return true;
}
#endif

#if defined(CONFIG_USE_EYE_STYLE_VB6824)
void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<uint8_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // TODO
                continue;
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<uint8_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    ESP_LOGD(TAG, "Audio processor feed: opus.size()=%u", (unsigned int)data.size());
                    audio_processor_->Feed(std::move(data));
                    continue;
                } else {
                    ESP_LOGE(TAG, "Failed to read audio data");
                }
            } else {
                ESP_LOGE(TAG, "Audio processor GetFeedSize returned 0");
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        break;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

#else
void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        break;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}
#endif

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                !audio_decode_queue_.empty() ||
                pending_voice_processing_start_  // 添加检查，确保在播放完成后能唤醒任务
#ifndef CONFIG_USE_EYE_STYLE_VB6824
                || (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE)
#endif
                ;
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue and output directly */
        if (!audio_decode_queue_.empty()) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();
            decode_pcm_buffer_.clear();  // 清空但保留容量
            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), decode_pcm_buffer_)) {
                // 重采样（如果需要）
#ifndef CONFIG_USE_EYE_STYLE_VB6824
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(decode_pcm_buffer_.size());
                    resample_buffer_.clear();  // 清空但保留容量
                    resample_buffer_.resize(target_size);
                    output_resampler_.Process(decode_pcm_buffer_.data(), decode_pcm_buffer_.size(), resample_buffer_.data());
                    
                    // 直接输出重采样后的数据
                    if (!codec_->output_enabled()) {
                        codec_->EnableOutput(true);
                        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                    }
                    codec_->OutputData(resample_buffer_);
                } else {
                    // 不需要重采样，直接输出解码后的数据
                    if (!codec_->output_enabled()) {
                        codec_->EnableOutput(true);
                        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                    }
                    codec_->OutputData(decode_pcm_buffer_);
                }
#else
                // VB6824模式：直接输出解码后的数据
                if (!codec_->output_enabled()) {
                    codec_->EnableOutput(true);
                    esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                }
                codec_->OutputData(decode_pcm_buffer_);
#endif
                
                // 更新最后输出时间
                last_output_time_ = std::chrono::steady_clock::now();
                debug_statistics_.playback_count++;
                
                // 检查是否需要启动语音处理
                if (pending_voice_processing_start_) {
                    bool should_start_voice_processing = false;
                    {
                        std::lock_guard<std::mutex> guard(audio_queue_mutex_);
                        if (audio_decode_queue_.empty()) {
                            // 解码队列为空，可以安全启动语音处理
                            ESP_LOGI(TAG, "Audio playback completed, enabling voice processing");
                            pending_voice_processing_start_ = false;
                            should_start_voice_processing = true;
                        } else {
                            ESP_LOGD(TAG, "Audio playback still in progress (%zu packets remaining), waiting...", audio_decode_queue_.size());
                        }
                    }
                    
                    if (should_start_voice_processing) {
                        ResetDecoder();
                        audio_input_need_warmup_ = true;
                        audio_processor_->Start();
                        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
                        
                    }
                }

#if CONFIG_USE_SERVER_AEC
                /* Record the timestamp for server AEC */
                if (packet->timestamp > 0) {
                    std::lock_guard<std::mutex> guard(audio_queue_mutex_);
                    timestamp_queue_.push_back(packet->timestamp);
                }
#endif
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
            }
            debug_statistics_.decode_count++;
            lock.lock();
        }
        
#ifndef CONFIG_USE_EYE_STYLE_VB6824
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            ESP_LOGD(TAG, "Opus encode: task->pcm.size()=%u", (unsigned int)task->pcm.size());
            
            // 修复内存泄漏：使用shared_ptr确保内存安全
            auto packet_ptr = packet.get();
            opus_encoder_->Encode(std::move(task->pcm), [packet_ptr](std::vector<uint8_t>&& opus) {
                if (packet_ptr) {
                    packet_ptr->payload = std::move(opus);
                }
            });

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                if (callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
#endif
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    // opus 编码不需要设置
#ifndef CONFIG_USE_EYE_STYLE_VB6824
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
#endif
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
#ifdef CONFIG_USE_EYE_STYLE_VB6824
    // 在 CONFIG_USE_EYE_STYLE_VB68204 模式下，不需要编码队列
    ESP_LOGW(TAG, "PushTaskToEncodeQueue called in CONFIG_USE_EYE_STYLE_VB6824 mode, ignoring");
    return;
#else
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    // 检查编码队列是否已满，如果满了就丢弃数据而不是无限等待
    if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
        ESP_LOGW(TAG, "Encode queue full (%u/%d), dropping audio data to prevent memory issues", 
                 (unsigned int)audio_encode_queue_.size(), MAX_ENCODE_TASKS_IN_QUEUE);
        // 丢弃数据，释放内存
        if (task) {
            task->pcm.clear();        // 清空PCM数据
            task->pcm.shrink_to_fit(); // 释放向量占用的内存
        }
        task.reset();
        audio_queue_cv_.notify_all();
        return;
    }
    
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
#endif
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    
    // 检查队列是否已满
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            ESP_LOGW(TAG, "Decode queue full (%u/%d), waiting for space...", 
                     (unsigned int)audio_decode_queue_.size(), MAX_DECODE_PACKETS_IN_QUEUE);
            audio_queue_cv_.wait(lock, [this]() { 
                return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; 
            });
        } else {
            ESP_LOGW(TAG, "Decode queue full (%u/%d), dropping packet to prevent memory issues", 
                     (unsigned int)audio_decode_queue_.size(), MAX_DECODE_PACKETS_IN_QUEUE);
            // 队列满时，显式释放数据包内存
            // 确保 payload 向量被清空，释放音频数据内存
            if (packet && !packet->payload.empty()) {
                ESP_LOGD(TAG, "Dropping packet with payload size: %u bytes", (unsigned int)packet->payload.size());
                packet->payload.clear();  // 清空音频数据
                packet->payload.shrink_to_fit();  // 释放向量占用的内存
            }
            audio_queue_cv_.notify_all();
            return false;
        }
    }
    
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGI(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable, bool force_stop) {
    ESP_LOGI(TAG, "%s voice processing (force_stop: %s)", enable ? "Enabling" : "Disabling", force_stop ? "true" : "false");
    
    if (enable) {
        // 确保音频处理器已初始化
        if (!audio_processor_initialized_) {
            ESP_LOGI(TAG, "Initializing audio processor...");
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
            audio_processor_initialized_ = true;
            ESP_LOGI(TAG, "Audio processor initialized successfully");
        }

        if (force_stop) {
            // 强制立即进入聆听模式
            ESP_LOGI(TAG, "Force stop mode - entering listening state immediately");
            /* We should make sure no audio is playing */
            ResetDecoder();
            audio_input_need_warmup_ = true;
            audio_processor_->Start();
            xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
            
        } else {
            // 正常模式 - 等待播放完成后再进入聆听模式
            ESP_LOGI(TAG, "Normal mode - checking audio playback status");
            bool should_start_immediately = false;
            {
                std::lock_guard<std::mutex> guard(audio_queue_mutex_);
                if (audio_decode_queue_.empty()) {
                    // 没有音频在播放，立即进入聆听模式
                    ESP_LOGI(TAG, "No audio playing, starting voice processing immediately");
                    should_start_immediately = true;
                } else {
                    // 有音频在播放，设置标志等待播放完成
                    ESP_LOGI(TAG, "Audio playback in progress (%zu packets), setting pending flag", audio_decode_queue_.size());
                    pending_voice_processing_start_ = true;
                    audio_queue_cv_.notify_all();  // 通知 OpusCodecTask 检查 pending 标志
                    // 不立即启动，等待播放完成后在 OpusCodecTask 中检查并启动
                }
            }
            
            if (should_start_immediately) {
                ResetDecoder();
                audio_input_need_warmup_ = true;
                audio_processor_->Start();
                xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
                
            } else {
                ESP_LOGI(TAG, "Voice processing will start after audio playback completes");
            }
        }
        
        // 最终状态验证
        ESP_LOGI(TAG, "Final state check - Audio processor running: %s, Pending start: %s", 
                  IsAudioProcessorRunning() ? "true" : "false",
                  pending_voice_processing_start_ ? "true" : "false");
                  
    } else {
        ESP_LOGI(TAG, "Disabling voice processing");
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        pending_voice_processing_start_ = false; // 清除待启动标志
        
    }
}

bool AudioService::VerifyVoiceProcessingState(bool expected_enabled) {
    bool is_running = IsAudioProcessorRunning();
    bool has_pending = pending_voice_processing_start_;
    
    ESP_LOGI(TAG, "=== Voice Processing State Verification ===");
    ESP_LOGI(TAG, "Expected enabled: %s", expected_enabled ? "true" : "false");
    ESP_LOGI(TAG, "Audio processor running: %s", is_running ? "true" : "false");
    ESP_LOGI(TAG, "Pending start flag: %s", has_pending ? "true" : "false");
    ESP_LOGI(TAG, "Audio processor initialized: %s", audio_processor_initialized_ ? "true" : "false");
    ESP_LOGI(TAG, "Audio input need warmup: %s", audio_input_need_warmup_ ? "true" : "false");
    
    if (expected_enabled) {
        // 期望启用语音处理
        if (is_running) {
            ESP_LOGI(TAG, "✓ Voice processing is running as expected");
            return true;
        } else if (has_pending) {
            ESP_LOGI(TAG, "⚠ Voice processing is pending (waiting for audio playback)");
            return true; // 这种情况也算正常
        } else {
            ESP_LOGE(TAG, "✗ Voice processing is not running and not pending");
            return false;
        }
    } else {
        // 期望禁用语音处理
        if (!is_running && !has_pending) {
            ESP_LOGI(TAG, "✓ Voice processing is disabled as expected");
            return true;
        } else {
            ESP_LOGE(TAG, "✗ Voice processing is still running or pending");
            return false;
        }
    }
}

bool AudioService::EnableVoiceProcessingWithRetry(bool enable, bool force_stop, int timeout_ms) {
    ESP_LOGI(TAG, "Enabling voice processing with timeout (enable: %s, force_stop: %s, timeout: %d ms)", 
              enable ? "true" : "false", force_stop ? "true" : "false", timeout_ms);
    
    if (!enable) {
        // 如果是禁用，直接调用原方法
        EnableVoiceProcessing(enable, force_stop);
        return true;
    }
    
    // 记录开始时间
    auto start_time = std::chrono::steady_clock::now();
    
    // 首先尝试正常启用
    EnableVoiceProcessing(enable, force_stop);
    
    // 等待并检查状态，直到超时
    while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(timeout_ms)) {
        // 检查是否已经成功
        if (IsAudioProcessorRunning()) {
            ESP_LOGI(TAG, "Voice processing started successfully within timeout");
            return true;
        }
        
        // 等待一小段时间再检查
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // 超时了，强制切换
    ESP_LOGW(TAG, "Timeout reached (%d ms), forcing voice processing start", timeout_ms);
    EnableVoiceProcessing(true, true); // 强制模式
    
    // 最终检查
    if (IsAudioProcessorRunning()) {
        ESP_LOGI(TAG, "Voice processing started successfully after force mode");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to start voice processing even after force mode");
        return false;
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& sound) {
    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = 16000;
        packet->frame_duration = 60;
        packet->payload.resize(payload_size);
        memcpy(packet->payload.data(), p3->payload, payload_size);
        p += payload_size;

        PushPacketToDecodeQueue(std::move(packet), true);
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
#ifndef CONFIG_USE_EYE_STYLE_VB6824
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_testing_queue_.empty();
#else
    return audio_decode_queue_.empty() && audio_testing_queue_.empty();
#endif
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
#ifndef CONFIG_USE_EYE_STYLE_VB6824
    audio_encode_queue_.clear();
#endif
    audio_decode_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}
