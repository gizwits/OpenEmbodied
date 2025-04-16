#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    virtual ~WebsocketProtocol();

    virtual void Start() override;
    virtual bool OpenAudioChannel() override;
    virtual void CloseAudioChannel() override;
    virtual bool IsAudioChannelOpened() const override;
    virtual void SendAudio(const std::vector<int16_t>& data) override;
    virtual void SendAudio(const std::vector<uint8_t>& data) override;

protected:
    virtual void SendText(const std::string& text) override;
    virtual void ParseServerHello(const cJSON* root);

private:
    WebSocket* websocket_ = nullptr;
    EventGroupHandle_t event_group_handle_;
};

#endif
