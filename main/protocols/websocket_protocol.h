#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <queue>
#include <memory>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    
    // TTS buffering
    bool tts_buffering_ = false;
    std::queue<std::unique_ptr<AudioStreamPacket>> audio_buffer_;
    size_t buffer_packet_count_ = 0;
    const size_t BUFFER_SIZE = 20;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    void ProcessBufferedAudio();
    void ClearAudioBuffer();
};

#endif
