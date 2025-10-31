#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include "player/player.h"
#include "server/giz_mqtt.h"
#include <string>
#include <mutex>
#include <deque>
#include "ntp.h"
#include "auth.h"
#include "watchdog.h"
#include <vector>
#include <memory>
#include "factory_test/test.h"
#include "factory_test/factory_test.h"

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    Player player_;
    UdpBroadcaster udp_broadcaster_;
    char trace_id_[33];  // 32 chars + null terminator
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    void GenerateTraceId();
    void ChangeBot(const char* id, const char* voice_id);
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback, const std::string& task_name = "unknown");
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void ResetDecoder();
    void PlaySound(const std::string_view& sound);
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void QuitTalking();
    void SetChatMode(int mode);
    int GetChatMode() const { return chat_mode_; }
    void CancelPlayMusic();
    void SendTextToAI(const std::string& text);
    bool IsTmpFactoryTestMode() const { return tmp_ft_mode_; }
    void SetIsTmpFactoryTestMode(bool is_tmp_ft_mode) { tmp_ft_mode_ = is_tmp_ft_mode; }
    
    // 工厂测试相关方法
    int StartRecordTest(int duration_seconds);
    int StartPlayTest(int duration_seconds);
    void StopRecordTest();
    // 工厂测试录制：供 AudioService 追加录制数据（Opus 原始片段）
    void AppendRecordedAudioData(const uint8_t* data, size_t size);


    // 关闭 wifi 的休眠
    void EnterSleepMode();
    void ExitSleepMode();
    void HandleNetError();

    const char* GetTraceId() const { return trace_id_; }
    void PlayMusic(const char* url);
    AudioService& GetAudioService() { return audio_service_; }
    bool IsNormalReset() const { return is_normal_reset_; }  // 获取重启状态
    bool IsSilentStartup() const { return is_silent_startup_; }  // 获取静默启动状态
    void ClearSilentStartup() { is_silent_startup_ = false; }  // 清除静默启动状态

    bool IsWebsocketWorking() const { return protocol_ ? protocol_->IsAudioChannelOpened() : false; }
    bool HasWebsocketError() const { return protocol_ ? protocol_->HasErrorOccurred() : false; }

private:
    Application();
    ~Application();
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::function<void()>>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::string last_error_message_;
    AudioService audio_service_;
    esp_timer_handle_t report_timer_handle_ = nullptr;
    int chat_mode_ = 1;
    bool has_emotion_ = false;
    bool tmp_ft_mode_ = false;
    std::chrono::steady_clock::time_point last_battery_check_time_;

    bool has_server_time_ = false;
    bool is_silent_startup_ = false;
    bool is_normal_reset_ = false;  // 重启状态标志：true=正常重启，false=异常重启
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    // 工厂测试录制相关变量
    bool record_test_active_ = false;
    int record_test_duration_seconds_ = 0;
    std::chrono::steady_clock::time_point record_test_start_time_;
    std::vector<uint8_t> recorded_audio_data_;
    std::mutex record_test_mutex_;
    esp_timer_handle_t record_timer_handle_ = nullptr;
    
    // 工厂测试播放相关变量
    bool play_test_active_ = false;
    int play_test_duration_seconds_ = 0;
    std::chrono::steady_clock::time_point play_test_start_time_;
    size_t play_test_data_index_ = 0;

    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;


    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void OnClockTimer();
    void initGizwitsServer();
    bool CheckBatteryLevel();
    void StartReportTimer();
    bool ProductTestCheck();

    void SetListeningMode(ListeningMode mode);

};

#endif // _APPLICATION_H_
