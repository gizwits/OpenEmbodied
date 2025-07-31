#ifndef _NTP_H_
#define _NTP_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <time.h>
#include <sys/time.h>
#include <string>
#include <functional>

// 阿里云NTP服务器
#define ALIYUN_NTP_SERVER1 "ntp.aliyun.com"
#define ALIYUN_NTP_SERVER2 "ntp1.aliyun.com"
#define ALIYUN_NTP_SERVER3 "ntp2.aliyun.com"
#define ALIYUN_NTP_SERVER4 "ntp3.aliyun.com"
#define ALIYUN_NTP_SERVER5 "ntp4.aliyun.com"
#define ALIYUN_NTP_SERVER6 "ntp5.aliyun.com"
#define ALIYUN_NTP_SERVER7 "ntp6.aliyun.com"
#define ALIYUN_NTP_SERVER8 "ntp7.aliyun.com"

// NTP同步事件
#define NTP_SYNC_SUCCESS_EVENT (1 << 0)
#define NTP_SYNC_FAILED_EVENT (1 << 1)

// NTP同步状态
enum NtpSyncStatus {
    NTP_SYNC_IDLE,
    NTP_SYNC_IN_PROGRESS,
    NTP_SYNC_SUCCESS,
    NTP_SYNC_FAILED
};

// NTP同步回调函数类型
typedef std::function<void(NtpSyncStatus status, const std::string& message)> NtpSyncCallback;

class NtpClient {
public:
    static NtpClient& GetInstance() {
        static NtpClient instance;
        return instance;
    }

    // 初始化NTP客户端
    esp_err_t Init();
    
    // 开始NTP同步
    esp_err_t StartSync();
    
    // 停止NTP同步
    void StopSync();
    
    // 获取当前时间戳
    time_t GetCurrentTimestamp();
    
    // 获取格式化的时间字符串
    std::string GetFormattedTime(const char* format = "%Y-%m-%d %H:%M:%S");
    
    // 获取NTP同步状态
    NtpSyncStatus GetSyncStatus() const { return sync_status_; }
    
    // 设置同步回调函数
    void SetSyncCallback(NtpSyncCallback callback) { sync_callback_ = callback; }
    
    // 检查是否已同步
    bool IsSynced() const { return sync_status_ == NTP_SYNC_SUCCESS; }
    
    // 获取最后同步时间
    time_t GetLastSyncTime() const { return last_sync_time_; }
    
    // 手动同步时间
    esp_err_t ManualSync();
    
    // 设置时区
    esp_err_t SetTimeZone(const char* timezone);
    
    // 获取时区
    const char* GetTimeZone() const { return timezone_; }
    
    // 检查是否需要同步（用于主循环调用）
    bool ShouldSync();
    
    // 处理同步（用于主循环调用）
    void ProcessSync();

private:
    NtpClient();
    ~NtpClient();
    
    // 删除拷贝构造函数和赋值运算符
    NtpClient(const NtpClient&) = delete;
    NtpClient& operator=(const NtpClient&) = delete;
    
    // SNTP事件处理函数
    static void SntpEventHandler(struct timeval *tv);
    
    // 初始化SNTP
    esp_err_t InitSntp();
    
    // 设置NTP服务器
    void SetNtpServers();
    
    // 同步状态更新
    void UpdateSyncStatus(NtpSyncStatus status, const std::string& message = "");
    
    // 成员变量
    NtpSyncStatus sync_status_;
    NtpSyncCallback sync_callback_;
    time_t last_sync_time_;
    time_t last_sync_attempt_;
    char timezone_[64];
    bool initialized_;
    EventGroupHandle_t ntp_event_group_;
    
    // 同步间隔（秒）
    static constexpr int SYNC_INTERVAL_SECONDS = 3600;  // 1小时
    
    static constexpr const char* TAG = "NtpClient";
};

#endif // _NTP_H_
