#include <vector>
#include <memory>
#include <string>
#include <fstream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "player.h"
#include "board.h"
#include <esp_log.h>
#include <cstring>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// #include "watchdog.h"

#define TAG "Player"
#define BUFFER_SIZE 2048  // 减小缓冲区：2KB足够处理几个数据包
#define CHUNK_SIZE 512    // 减小每次读取大小：512字节，减少内存峰值
#define OPUS_FRAME_DURATION_MS 60  // 每个数据包包含60ms的音频
#define MAX_BUFFERED_BYTES 1200    // 最多缓存约2-3个数据包的数据（假设每个包约400字节）

struct Player::Impl {
    char* buffer;
    size_t buffer_pos;
    size_t buffer_size;
    bool is_downloading_;
    std::function<void(std::vector<uint8_t>&&)> packet_callback;
    std::function<size_t()> queue_size_callback;  // 查询队列大小的回调
    size_t packets_processed;  // 已处理的数据包数量

    Impl() : buffer_pos(0), buffer_size(0), is_downloading_(false), packets_processed(0) {
        buffer = new char[BUFFER_SIZE];
    }

    ~Impl() {
        stop();
        delete[] buffer;
    }

    void process_buffer() {
        while (buffer_pos >= 4) {  // 至少需要4字节头部
            // 解析头部
            uint16_t payload_size = (buffer[2] << 8) | buffer[3];
            size_t total_size = 4 + payload_size;
            
            // 验证数据包大小合理性（防止解析错误）
            if (payload_size > 2000 || total_size > buffer_pos) {
                if (payload_size > 2000) {
                    ESP_LOGE(TAG, "Invalid payload_size: %u, resetting buffer", payload_size);
                    buffer_pos = 0;
                }
                break;  // 数据包不完整或无效，等待更多数据
            }
            
            if (buffer_pos < total_size) {
                // 数据包不完整，等待更多数据
                break;
            }
            
            // 提取Opus数据（跳过4字节头部）
            std::vector<uint8_t> opus_data(buffer + 4, buffer + total_size);
            
            // 通过回调发送Opus数据（使用move避免复制）
            if (packet_callback) {
                packet_callback(std::move(opus_data));
                packets_processed++;
            }
            
            // 移动缓冲区
            memmove(buffer, buffer + total_size, buffer_pos - total_size);
            buffer_pos -= total_size;
        }
    }

    bool read_chunk(std::unique_ptr<Http>& http) {
        // 如果缓冲区已经有足够的数据，等待处理（基于实际数据量而非固定值）
        if (buffer_pos > MAX_BUFFERED_BYTES) {
            ESP_LOGD(TAG, "Buffer has enough data (%u bytes), waiting...", (unsigned int)buffer_pos);
            vTaskDelay(pdMS_TO_TICKS(10));  // 等待10ms让处理跟上
            // 继续处理缓冲区，不读取新数据
            process_buffer();
            return true;
        }

        // 确保有足够空间读取下一个chunk
        if (buffer_pos + CHUNK_SIZE > BUFFER_SIZE) {
            // 缓冲区快满了，先处理已有数据
            process_buffer();
            // 如果处理后仍然空间不足，说明数据包太大或处理太慢
            if (buffer_pos + CHUNK_SIZE > BUFFER_SIZE) {
                ESP_LOGW(TAG, "Buffer nearly full (%u bytes), waiting for processing...", (unsigned int)buffer_pos);
                vTaskDelay(pdMS_TO_TICKS(10));
                return true;
            }
        }

        char chunk[CHUNK_SIZE];
        int bytes_read = http->Read(chunk, CHUNK_SIZE);
        if (bytes_read <= 0) {
            // 读取结束或出错，处理剩余缓冲区数据
            if (buffer_pos > 0) {
                process_buffer();
            }
            return false;  // 读取结束或出错
        }

        // 将数据复制到缓冲区
        memcpy(buffer + buffer_pos, chunk, bytes_read);
        buffer_pos += bytes_read;
        buffer_size = buffer_pos;

        // 处理缓冲区中的数据包
        process_buffer();
        return true;
    }

    void stop() {
        ESP_LOGI(TAG, "Player stop called, cleaning up...");
        is_downloading_ = false;
    }

    void setPacketCallback(std::function<void(std::vector<uint8_t>&&)> callback) {
        packet_callback = callback;
    }

    void setQueueSizeCallback(std::function<size_t()> callback) {
        queue_size_callback = callback;
    }

    bool processMP3Stream(const char* url) {
        ESP_LOGI(TAG, "processMP3Stream: %s", url);
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(4);
        
        // 设置接收限流回调：直接检查音频解码队列（最准确的限流方式）
        if (queue_size_callback) {
            http->SetCanReceiveCallback([this]() {
                size_t queue_size = queue_size_callback();
                const size_t max_queue_size = 
#ifdef CONFIG_IDF_TARGET_ESP32S3
                    (10000 / OPUS_FRAME_DURATION_MS);
#else
                    (3600 / OPUS_FRAME_DURATION_MS);
#endif
                // 如果队列超过50%满，暂停接收
                bool can_receive = max_queue_size == 0 || queue_size < max_queue_size * 0.5;
                if (!can_receive) {
                    ESP_LOGD(TAG, "Decode queue high (%u/%u, %.1f%%), pausing HTTP receive",
                             (unsigned int)queue_size, (unsigned int)max_queue_size,
                             (float)queue_size / (float)max_queue_size * 100.0f);
                }
                return can_receive;
            });
        } else {
            // 如果没有队列回调，使用缓冲区大小限制作为后备方案
            http->SetMaxBufferSize(6 * 1024);
        }
        
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection");
            
            return false;
        }


        size_t content_length = http->GetBodyLength();
        if (content_length == 0) {
            ESP_LOGE(TAG, "Failed to get content length");
            
            return false;
        }

        auto status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "status_code: %d", status_code);

        is_downloading_ = true;
        packets_processed = 0;

        // 流式读取数据
        while (is_downloading_) {
            if (!read_chunk(http)) {
                break;
            }
            // Watchdog::GetInstance().Reset();
        }


        // 清理缓冲区
        buffer_pos = 0;
        buffer_size = 0;
        packets_processed = 0;

        
        return true;
    }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() = default;

bool Player::IsDownloading() const {
    return impl_->is_downloading_;
}

void Player::setPacketCallback(std::function<void(std::vector<uint8_t>&&)> callback) {
    impl_->setPacketCallback(callback);
}

void Player::setQueueSizeCallback(std::function<size_t()> callback) {
    impl_->setQueueSizeCallback(callback);
}

bool Player::processMP3Stream(const char* url) {
    return impl_->processMP3Stream(url);
}

void Player::stop() {
    impl_->stop();
}
