#include "ntp.h"
#include <esp_netif.h>
#include <esp_sntp.h>
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

// 同步超时时间（秒）
#define NTP_SYNC_TIMEOUT_SECONDS 30

NtpClient::NtpClient() 
    : sync_status_(NTP_SYNC_IDLE)
    , last_sync_time_(0)
    , last_sync_attempt_(0)
    , initialized_(false)
    , ntp_event_group_(nullptr) {
    strcpy(timezone_, "CST-8");
}

NtpClient::~NtpClient() {
    StopSync();
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
    
    // 初始化SNTP
    esp_err_t ret = InitSntp();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 设置时区
    ret = SetTimeZone(timezone_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set timezone: %s", esp_err_to_name(ret));
        return ret;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "NTP client initialized successfully");
    return ESP_OK;
}

esp_err_t NtpClient::InitSntp() {
    // 设置SNTP事件处理函数
    sntp_set_time_sync_notification_cb(SntpEventHandler);
    
    // 设置SNTP操作模式
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 设置NTP服务器
    SetNtpServers();
    
    // 启动SNTP
    sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized");
    return ESP_OK;
}

void NtpClient::SetNtpServers() {
    // 设置阿里云NTP服务器
    sntp_setservername(0, ALIYUN_NTP_SERVER1);
    sntp_setservername(1, ALIYUN_NTP_SERVER2);
    sntp_setservername(2, ALIYUN_NTP_SERVER3);
    
    ESP_LOGI(TAG, "NTP servers set: %s, %s, %s", 
             ALIYUN_NTP_SERVER1, ALIYUN_NTP_SERVER2, ALIYUN_NTP_SERVER3);
}

esp_err_t NtpClient::StartSync() {
    if (!initialized_) {
        ESP_LOGE(TAG, "NTP client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not connected, cannot start NTP sync");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "NTP sync started");
    return ESP_OK;
}

void NtpClient::StopSync() {
    // ESP-IDF中没有sntp_stop函数，直接更新状态
    UpdateSyncStatus(NTP_SYNC_IDLE, "NTP sync stopped");
    ESP_LOGI(TAG, "NTP sync stopped");
}

esp_err_t NtpClient::ManualSync() {
    if (!initialized_) {
        ESP_LOGE(TAG, "NTP client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi not connected, cannot perform manual sync");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 清除之前的事件
    xEventGroupClearBits(ntp_event_group_, NTP_SYNC_SUCCESS_EVENT | NTP_SYNC_FAILED_EVENT);
    
    // 更新状态为正在同步
    UpdateSyncStatus(NTP_SYNC_IN_PROGRESS, "NTP sync in progress");
    
    // 记录同步尝试时间
    last_sync_attempt_ = GetCurrentTimestamp();
    
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
    
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
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

void NtpClient::SntpEventHandler(struct timeval *tv) {
    NtpClient& instance = NtpClient::GetInstance();
    
    if (tv) {
        ESP_LOGI(TAG, "SNTP sync completed");
        instance.UpdateSyncStatus(NTP_SYNC_SUCCESS, "SNTP sync completed");
        xEventGroupSetBits(instance.ntp_event_group_, NTP_SYNC_SUCCESS_EVENT);
    } else {
        ESP_LOGE(TAG, "SNTP sync failed");
        instance.UpdateSyncStatus(NTP_SYNC_FAILED, "SNTP sync failed");
        xEventGroupSetBits(instance.ntp_event_group_, NTP_SYNC_FAILED_EVENT);
    }
}
