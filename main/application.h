#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include "player/player.h"


#include <string>
#include <mutex>
#include <list>
#include <vector>
#include <condition_variable>
#include <memory>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "protocol.h"
#include "server/giz_mqtt.h"
#include "ota.h"
#include "background_task.h"
#include "ntp.h"
#if CONFIG_USE_AUDIO_PROCESSOR
#include "audio_processor.h"
#endif

#if CONFIG_USE_WAKE_WORD_DETECT
#include "wake_word_detect.h"
#endif

#define SCHEDULE_EVENT (1 << 0)
#define AUDIO_INPUT_READY_EVENT (1 << 1)
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)
#define CHECK_NEW_VERSION_DONE_EVENT (1 << 3)

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateFatalError,
    kDeviceStatePowerOff
};

#define OPUS_FRAME_DURATION_MS 60

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    char trace_id_[33];  // 32 chars + null terminator

    Player player_;

    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    void QuitTalking();
    void Start();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return voice_detected_; }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void ChangeBot(const char* id, const char* voice_id);
    void AbortSpeaking(AbortReason reason);
    void PlayMusic(const char* url);
    void CancelPlayMusic();
    void ResetDecoder();
    void ToggleChatState();
    void StartListening();
    void SetChatMode(int mode);
    int GetChatMode() const { return chat_mode_; }
    void StopListening();
    void UpdateIotStates();
    void Reboot();
    void SendMessage(const std::string& message);
    void WakeWordInvoke(const std::string& wake_word);
    void PlaySound(const std::string_view& sound);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SendTextToAI(const std::string& text);

    const char* GetTraceId() const { return trace_id_; }
    void GenerateTraceId();
private:
    Application();
    ~Application();

#if CONFIG_USE_WAKE_WORD_DETECT
    WakeWordDetect wake_word_detect_;
#endif
#if CONFIG_USE_AUDIO_PROCESSOR
    std::unique_ptr<AudioProcessor> audio_processor_;
#endif
    Ota ota_;
    std::mutex mutex_;
    std::list<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;

    int chat_mode_ = 1;
    bool realtime_chat_is_start_ = false;
    

    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t report_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    bool aborted_ = false;
    bool voice_detected_ = false;
    bool busy_decoding_audio_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    // Audio encode / decode
    TaskHandle_t audio_loop_task_handle_ = nullptr;
    BackgroundTask* background_task_ = nullptr;
    std::chrono::steady_clock::time_point last_output_time_;
    std::list<AudioStreamPacket> audio_decode_queue_;
    std::condition_variable audio_decode_cv_;

    // 新增：用于维护音频包的timestamp队列
    std::list<uint32_t> timestamp_queue_;
    std::mutex timestamp_mutex_;
    std::atomic<uint32_t> last_output_timestamp_ = 0;

    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;

    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    void MainEventLoop();
    void OnAudioInput();
    void OnAudioOutput();
    void ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples);
#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
    void ReadAudio(std::vector<uint8_t>& opus, int sample_rate, int samples);
#endif

    void WriteAudio(std::vector<int16_t>& data, int sample_rate);
#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
    void WriteAudio(std::vector<uint8_t>& opus);
#endif
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckNewVersion();
    void ShowActivationCode();
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
    void AudioLoop();
    void StartReportTimer();
};

#endif // _APPLICATION_H_
