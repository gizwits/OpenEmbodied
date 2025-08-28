#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"
#include "esp_heap_caps.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <optional>
#include <chrono>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();

    virtual ~WebsocketProtocol();

    virtual bool Start() override;
    virtual bool OpenAudioChannel() override;
    virtual void SendStopListening() override;
    virtual void SendTextToAI(const std::string& text);
    virtual void CloseAudioChannel() override;
    virtual bool IsAudioChannelOpened() const override;
    virtual void SendAudio(const AudioStreamPacket& packet) override;
    virtual bool HasErrorOccurred() const override;
    virtual void HandleReconnect();
    
private:
    std::unique_ptr<WebSocket> websocket_;
    EventGroupHandle_t event_group_handle_;
    TaskHandle_t close_task_handle_ = nullptr;
    std::vector<AudioStreamPacket> packet_cache_;
    int cached_packet_count_ = 0;
    
    std::string message_cache_;
    std::vector<uint8_t> audio_data_buffer_;  // Reuse buffer for Ogg data
    std::unique_ptr<char[]> base64_buffer_;  // Reuse buffer for base64 encoding
    size_t base64_buffer_size_ = 0;  // Current size of base64 buffer
    std::string message_buffer_;  // Reuse buffer for message construction
    
    // 用户说话结束时间戳，用于chat_mode==1时忽略1秒内的音频上传
    std::chrono::steady_clock::time_point speech_stopped_timestamp_;
    bool speech_stopped_recorded_ = false;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    static void CloseAudioChannelTask(void* param);
    void SwitchToSpeaking();

    static void ReconnectTask(void* param);

    static const int MAX_RECONNECT_ATTEMPTS = 3; // 最大重连数
    static const int RECONNECT_INTERVAL_MS = 5000; // 重连间隔时间
    int reconnect_attempts_ = 0; // 当前重连次数
    bool is_reconnecting_ = false; // 是否正在重连
    bool should_reconnect_ = true; // 控制是否需要重连
};

#endif