#include "ntp.h"
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <cstring>
#include <cstdint>
#include <inttypes.h>

// 同步超时时间（秒）
#define NTP_SYNC_TIMEOUT_SECONDS 30

// NTP 时间戳起始点：1900年1月1日 00:00:00 UTC
#define NTP_TIMESTAMP_DELTA 2208988800ULL

NtpClient::NtpClient() 
    : sync_status_(NTP_SYNC_IDLE)
    , last_sync_time_(0)
    , last_sync_attempt_(0)
    , initialized_(false)
    , ntp_event_group_(nullptr)
    , current_server_index_(0)
    , sync_task_handle_(nullptr) {
    strcpy(timezone_, "CST-8");
}

NtpClient::~NtpClient() {
    StopSync();
    if (udp_client_) {
        udp_client_->Disconnect();
        udp_client_.reset();
    }
    if (ntp_event_group_) {
        vEventGroupDelete(ntp_event_group_);
    }
}

esp_err_t NtpClient::Init() {
    if (initialized_) {
        ESP_LOGW(TAG, "NTP client already initialized");
        return ESP_OK;
    }
    
    // 创建事件组
    ntp_event_group_ = xEventGroupCreate();
    if (!ntp_event_group_) {
        ESP_LOGE(TAG, "Failed to create NTP event group");
        return ESP_ERR_NO_MEM;
    }
    
    // 设置时区
    esp_err_t ret = SetTimeZone(timezone_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timezone: %s", esp_err_to_name(ret));
        return ret;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "NTP client initialized successfully");
    return ESP_OK;
}

esp_err_t NtpClient::StartSync() {
    if (!initialized_) {
        ESP_LOGE(TAG, "NTP client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!IsNetworkConnected()) {
        ESP_LOGE(TAG, "Network not connected, cannot start NTP sync");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "NTP sync started");
    return ESP_OK;
}

void NtpClient::StopSync() {
    if (sync_task_handle_) {
        // 通知任务退出
        sync_status_ = NTP_SYNC_IDLE;
        vTaskDelay(pdMS_TO_TICKS(100));
        sync_task_handle_ = nullptr;
    }
    
    if (udp_client_) {
        udp_client_->Disconnect();
        udp_client_.reset();
    }
    
    UpdateSyncStatus(NTP_SYNC_IDLE, "NTP sync stopped");
    ESP_LOGI(TAG, "NTP sync stopped");
}

esp_err_t NtpClient::ManualSync() {
    if (!initialized_) {
        ESP_LOGE(TAG, "NTP client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!IsNetworkConnected()) {
        ESP_LOGE(TAG, "Network not connected, cannot perform manual sync");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 如果已经在同步中，直接返回
    if (sync_status_ == NTP_SYNC_IN_PROGRESS) {
        ESP_LOGW(TAG, "NTP sync already in progress");
        return ESP_OK;
    }
    
    // 清除之前的事件
    xEventGroupClearBits(ntp_event_group_, NTP_SYNC_SUCCESS_EVENT | NTP_SYNC_FAILED_EVENT);
    
    // 更新状态为正在同步
    UpdateSyncStatus(NTP_SYNC_IN_PROGRESS, "NTP sync in progress");
    
    // 记录同步尝试时间
    last_sync_attempt_ = GetCurrentTimestamp();
    
    // 创建同步任务
    if (sync_task_handle_ == nullptr) {
        xTaskCreate([](void* arg) {
            NtpClient* client = static_cast<NtpClient*>(arg);
            client->PerformSync();
            client->sync_task_handle_ = nullptr;
            vTaskDelete(nullptr);
        }, "ntp_sync", 4096, this, 5, &sync_task_handle_);
    }
    
    ESP_LOGI(TAG, "Manual NTP sync started");
    return ESP_OK;
}

esp_err_t NtpClient::SetTimeZone(const char* timezone) {
    if (!timezone) {
        ESP_LOGE(TAG, "Invalid timezone parameter");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = setenv("TZ", timezone, 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set TZ environment variable");
        return ESP_FAIL;
    }
    
    tzset();
    strncpy(timezone_, timezone, sizeof(timezone_) - 1);
    timezone_[sizeof(timezone_) - 1] = '\0';
    
    ESP_LOGI(TAG, "Timezone set to: %s", timezone_);
    return ESP_OK;
}

time_t NtpClient::GetCurrentTimestamp() {
    time_t now;
    time(&now);
    return now;
}

std::string NtpClient::GetFormattedTime(const char* format) {
    time_t now = GetCurrentTimestamp();
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char time_str[64];
    strftime(time_str, sizeof(time_str), format, &timeinfo);
    
    return std::string(time_str);
}

bool NtpClient::ShouldSync() {
    if (!initialized_ || sync_status_ == NTP_SYNC_IN_PROGRESS) {
        return false;
    }
    
    if (!IsNetworkConnected()) {
        return false;
    }
    
    // 检查是否从未同步过
    if (last_sync_time_ == 0) {
        return true;
    }
    
    // 检查是否超过同步间隔
    time_t now = GetCurrentTimestamp();
    if (now - last_sync_time_ >= SYNC_INTERVAL_SECONDS) {
        return true;
    }
    
    return false;
}

void NtpClient::ProcessSync() {
    if (sync_status_ == NTP_SYNC_IN_PROGRESS) {
        // 检查同步结果
        EventBits_t bits = xEventGroupGetBits(ntp_event_group_);
        
        if (bits & NTP_SYNC_SUCCESS_EVENT) {
            ESP_LOGI(TAG, "NTP sync completed successfully");
            xEventGroupClearBits(ntp_event_group_, NTP_SYNC_SUCCESS_EVENT);
        } else if (bits & NTP_SYNC_FAILED_EVENT) {
            ESP_LOGE(TAG, "NTP sync failed");
            xEventGroupClearBits(ntp_event_group_, NTP_SYNC_FAILED_EVENT);
        } else {
            // 检查是否超时
            time_t now = GetCurrentTimestamp();
            if (now - last_sync_attempt_ >= NTP_SYNC_TIMEOUT_SECONDS) {
                ESP_LOGE(TAG, "NTP sync timeout");
                UpdateSyncStatus(NTP_SYNC_FAILED, "NTP sync timeout");
            }
        }
    } else if (ShouldSync()) {
        // 开始新的同步
        ManualSync();
    }
}

void NtpClient::UpdateSyncStatus(NtpSyncStatus status, const std::string& message) {
    sync_status_ = status;
    
    if (status == NTP_SYNC_SUCCESS) {
        last_sync_time_ = GetCurrentTimestamp();
        ESP_LOGI(TAG, "NTP sync successful at %s", GetFormattedTime().c_str());
    } else if (status == NTP_SYNC_FAILED) {
        ESP_LOGE(TAG, "NTP sync failed: %s", message.c_str());
    }
    
    // 调用回调函数
    if (sync_callback_) {
        sync_callback_(status, message);
    }
}

bool NtpClient::IsNetworkConnected() {
    // TODO 增加 4g 和 wifi 的链接判断
    return true;
}

std::string NtpClient::BuildNtpRequest() {
    // NTP 数据包：48 字节
    std::string packet(48, 0);
    
    // 字节 0: LI (2 bits) + VN (3 bits) + Mode (3 bits)
    // LI = 0 (无警告), VN = 4 (NTP版本4), Mode = 3 (客户端模式)
    // 0x23 = 00100011 = LI=00, VN=100, Mode=011
    packet[0] = 0x23;
    
    // 其他字段保持为0（对于客户端请求）
    
    return packet;
}

bool NtpClient::ParseNtpResponse(const std::string& data, time_t& timestamp) {
    if (data.size() < 48) {
        ESP_LOGE(TAG, "NTP response too short: %zu bytes", data.size());
        return false;
    }
    
    // 检查响应模式（应该是服务器模式 4）
    uint8_t mode = data[0] & 0x07;
    if (mode != 4) {
        ESP_LOGE(TAG, "Invalid NTP response mode: %d", mode);
        return false;
    }
    
    // 提取传输时间戳（字节 40-43）
    uint32_t seconds = 0;
    seconds |= (static_cast<uint8_t>(data[40]) << 24);
    seconds |= (static_cast<uint8_t>(data[41]) << 16);
    seconds |= (static_cast<uint8_t>(data[42]) << 8);
    seconds |= static_cast<uint8_t>(data[43]);
    
    // NTP 时间戳是从 1900 年 1 月 1 日开始的秒数
    // 需要转换为 Unix 时间戳（从 1970 年 1 月 1 日）
    // 差值 = 2208988800 秒
    if (seconds >= NTP_TIMESTAMP_DELTA) {
        timestamp = static_cast<time_t>(seconds - NTP_TIMESTAMP_DELTA);
    } else {
        ESP_LOGE(TAG, "Invalid NTP timestamp: %" PRIu32, seconds);
        return false;
    }
    
    return true;
}

void NtpClient::OnNtpResponse(const std::string& data) {
    time_t timestamp;
    if (!ParseNtpResponse(data, timestamp)) {
        ESP_LOGE(TAG, "Failed to parse NTP response");
        xEventGroupSetBits(ntp_event_group_, NTP_SYNC_FAILED_EVENT);
        return;
    }
    
    // 设置系统时间
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGE(TAG, "Failed to set system time");
        xEventGroupSetBits(ntp_event_group_, NTP_SYNC_FAILED_EVENT);
        return;
    }
    
    ESP_LOGI(TAG, "System time set to: %s", GetFormattedTime().c_str());
    
    // 更新状态
    UpdateSyncStatus(NTP_SYNC_SUCCESS, "NTP sync completed");
    xEventGroupSetBits(ntp_event_group_, NTP_SYNC_SUCCESS_EVENT);
    
    // 注意：不能在这里直接断开和删除 UDP 客户端
    // 因为回调是在接收任务线程中执行的，如果立即删除会导致接收任务
    // 在尝试设置事件组位时访问已删除的事件组，从而触发断言失败
    // 断开和清理将在 PerformSync 中处理
}

void NtpClient::PerformSync() {
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network interface not available");
        UpdateSyncStatus(NTP_SYNC_FAILED, "Network interface not available");
        xEventGroupSetBits(ntp_event_group_, NTP_SYNC_FAILED_EVENT);
        return;
    }
    
    // 尝试每个 NTP 服务器
    bool sync_success = false;
    for (int i = 0; i < NTP_SERVER_COUNT && sync_status_ == NTP_SYNC_IN_PROGRESS; i++) {
        current_server_index_ = i;
        const char* server = NTP_SERVERS[i];
        
        ESP_LOGI(TAG, "Trying NTP server %d: %s", i + 1, server);
        
        // 创建 UDP 客户端
        udp_client_ = network->CreateUdp(6);
        if (!udp_client_) {
            ESP_LOGE(TAG, "Failed to create UDP client");
            continue;
        }
        
        // 设置接收回调
        udp_client_->OnMessage([this](const std::string& data) {
            this->OnNtpResponse(data);
        });
        
        // 连接到 NTP 服务器
        if (!udp_client_->Connect(server, NTP_PORT)) {
            ESP_LOGE(TAG, "Failed to connect to %s:%d", server, NTP_PORT);
            udp_client_.reset();
            continue;
        }
        
        // 构造并发送 NTP 请求
        std::string request = BuildNtpRequest();
        int sent = udp_client_->Send(request);
        if (sent <= 0) {
            ESP_LOGE(TAG, "Failed to send NTP request");
            udp_client_->Disconnect();
            udp_client_.reset();
            continue;
        }
        
        ESP_LOGI(TAG, "NTP request sent to %s", server);
        
        // 等待响应（最多等待 5 秒）
        int wait_count = 0;
        while (sync_status_ == NTP_SYNC_IN_PROGRESS && wait_count < 50) {
            EventBits_t bits = xEventGroupGetBits(ntp_event_group_);
            if (bits & NTP_SYNC_SUCCESS_EVENT) {
                sync_success = true;
                break;
            } else if (bits & NTP_SYNC_FAILED_EVENT) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }
        
        if (sync_success) {
            // 同步成功，断开连接并清理
            if (udp_client_) {
                udp_client_->Disconnect();
                // 等待一小段时间，确保接收任务完全退出
                vTaskDelay(pdMS_TO_TICKS(100));
                udp_client_.reset();
            }
            break;
        }
        
        // 断开连接，尝试下一个服务器
        if (udp_client_) {
            udp_client_->Disconnect();
            // 等待一小段时间，确保接收任务完全退出
            vTaskDelay(pdMS_TO_TICKS(100));
            udp_client_.reset();
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (!sync_success && sync_status_ == NTP_SYNC_IN_PROGRESS) {
        ESP_LOGE(TAG, "All NTP servers failed");
        UpdateSyncStatus(NTP_SYNC_FAILED, "All NTP servers failed");
        xEventGroupSetBits(ntp_event_group_, NTP_SYNC_FAILED_EVENT);
        
        // 清理 UDP 客户端
        if (udp_client_) {
            udp_client_->Disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            udp_client_.reset();
        }
    }
}
