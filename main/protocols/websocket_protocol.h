#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_

#include "protocol.h"
#include "esp_heap_caps.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <optional>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();

    virtual ~WebsocketProtocol();

    virtual bool Start() override;
    virtual bool OpenAudioChannel() override;
    virtual void SendStopListening() override;
    virtual void CloseAudioChannel() override;
    virtual bool IsAudioChannelOpened() const override;
    virtual void SendAudio(const AudioStreamPacket& packet) override;

private:
    WebSocket* websocket_ = nullptr;
    EventGroupHandle_t event_group_handle_;
    TaskHandle_t close_task_handle_ = nullptr;
    std::optional<AudioStreamPacket> packet_cache_;
    
    std::string message_cache_;
    std::vector<uint8_t> audio_data_buffer_;  // Reuse buffer for Ogg data
    std::unique_ptr<char[]> base64_buffer_;  // Reuse buffer for base64 encoding
    size_t base64_buffer_size_ = 0;  // Current size of base64 buffer
    std::string message_buffer_;  // Reuse buffer for message construction
   

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    static void CloseAudioChannelTask(void* param);
};

#endif