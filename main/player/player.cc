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
#define BUFFER_SIZE 4096
#define MAX_PACKET_SIZE 1500
#define CHUNK_SIZE 1024  // 每次读取1KB数据
#define PACKET_DURATION_MS 55  // 每个数据包包含60ms的音频
#define MAX_BUFFERED_PACKETS 3  // 最多缓存3个数据包

struct Player::Impl {
    char* buffer;
    size_t buffer_pos;
    size_t buffer_size;
    bool is_downloading_;
    std::function<void(const std::vector<uint8_t>&)> packet_callback;
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
            
            if (buffer_pos < total_size) {
                // 数据包不完整，等待更多数据
                break;
            }
            
            // 提取Opus数据（跳过4字节头部）
            std::vector<uint8_t> opus_data(buffer + 4, buffer + total_size);
            
            // 通过回调发送Opus数据
            if (packet_callback) {
                packet_callback(opus_data);
                packets_processed++;
                
                // 计算应该等待的时间
                TickType_t wait_ticks = pdMS_TO_TICKS(PACKET_DURATION_MS);
                vTaskDelay(wait_ticks);
            }
            
            // 移动缓冲区
            memmove(buffer, buffer + total_size, buffer_pos - total_size);
            buffer_pos -= total_size;
        }
    }

    bool read_chunk(std::unique_ptr<Http>& http) {
        // 如果缓冲区已经有足够的数据，等待处理
        if (buffer_pos > (MAX_PACKET_SIZE + 4) * MAX_BUFFERED_PACKETS) {
            ESP_LOGD(TAG, "Buffer has enough data, waiting...");
            vTaskDelay(pdMS_TO_TICKS(10));  // 等待10ms
            return true;
        }

        char chunk[CHUNK_SIZE];
        int bytes_read = http->Read(chunk, CHUNK_SIZE);
        if (bytes_read <= 0) {
            return false;  // 读取结束或出错
        }

        // 检查缓冲区是否足够
        if (buffer_pos + bytes_read > BUFFER_SIZE) {
            ESP_LOGE(TAG, "Buffer overflow");
            return false;
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

    void setPacketCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        packet_callback = callback;
    }

    bool processMP3Stream(const char* url) {
        ESP_LOGI(TAG, "processMP3Stream: %s", url);
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(5);
        
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

void Player::setPacketCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
    impl_->setPacketCallback(callback);
}

bool Player::processMP3Stream(const char* url) {
    return impl_->processMP3Stream(url);
}

void Player::stop() {
    impl_->stop();
}
