#pragma once

#include <string>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <ml307_mqtt.h>
#include "cJSON.h"

#define MQTT_REQUEST_FAILURE_COUNT 10

// MQTT message structure
typedef struct {
    char* topic;
    int topic_len;
    char* payload;
    int payload_len;
    char* data;
    int data_len;
    int qos;
} mqtt_msg_t;

// RTC parameters structure
typedef struct {
    char bot_id[64];
    char voice_id[64];
    char user_id[64];
    char conv_id[64];
    char access_token[256];
    int expires_in;
} room_params_t;

class MqttClient {
public:
    struct Config {
        std::string mqtt_address;
        int mqtt_port;
    };

    static MqttClient& getInstance() {
        static MqttClient instance;
        return instance;
    }

    bool initialize(const Config& config);
    bool publish(const std::string& topic, const std::string& payload);
    bool subscribe(const std::string& topic);
    void setMessageCallback(std::function<void(const std::string&, const std::string&)> callback);
    void sendTokenReport(int total, int output, int input);
    bool getRoomInfo();
    int sendResetToCloud();
    int getPublishedId();
    void OnRoomParamsUpdated(std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> callback);
    void deinit();

    MqttClient() = default;
    ~MqttClient() = default;
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

private:
    Mqtt* mqtt_ = nullptr;
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> room_params_updated_callback_;
    std::function<void(const std::string&, const std::string&)> message_callback_;
    QueueHandle_t message_queue_ = nullptr;
    SemaphoreHandle_t mqtt_sem_ = nullptr;
    TimerHandle_t timer_ = nullptr;
    int mqtt_event_ = 0;
    int mqtt_request_failure_count_ = 0;
    int mqtt_published_id_ = -1;
    bool is_first_connect_ = true;
    std::string password_;
    std::string endpoint_;
    std::string client_id_;
    std::string username_;

    static void messageReceiveHandler(void* arg);
    static void messageResendHandler(void* arg);
    static void timerCallback(TimerHandle_t xTimer);
    bool parseRealtimeAgent(const char* in_str, int in_len, room_params_t* params);
    bool parseM2MCtrlMsg(const char* in_str, int in_len);
    void handleMqttMessage(mqtt_msg_t* msg);
};
