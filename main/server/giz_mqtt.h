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
#include "protocols/protocol.h"

#define GAGENT_PROTOCOL_VERSION     (0x00000003)
#define HI_CMD_PAYLOAD93            0x0093
#define HI_CMD_UPLOADACK94          0x0094
#define HI_CMD_MQTT_RESET           0x021E
#define MQTT_REQUEST_FAILURE_COUNT 10


struct Attr {
    std::string name;
    int byte_offset;
    int bit_offset;
    int len;
    std::string unit; // "bit" or "byte"
    // 可扩展更多字段
};

static std::vector<Attr> g_attrs;
static std::once_flag g_attrs_once;


#define hexdump(pName, buf, len) do { \
    if (pName) { \
        printf("%s: ", pName); \
    } \
    for (size_t i = 0; i < len; i++) { \
        printf("%02X ", ((const uint8_t *)(buf))[i]); \
    } \
    printf("\n"); \
} while (0)

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
    char voice_lang[64];
    char api_domain[256];
    int expires_in;
} room_params_t;

class MqttClient {
public:

    static MqttClient& getInstance() {
        static MqttClient instance;
        return instance;
    }

    bool initialize();
    static void InitAttrsFromJson();
    bool publish(const std::string& topic, const std::string& payload);
    bool subscribe(const std::string& topic);
    void setMessageCallback(std::function<void(const std::string&, const std::string&)> callback);
    void sendTokenReport(int total, int output, int input);
    bool getRoomInfo();
    int sendResetToCloud();
    int getPublishedId();
    void sendOtaProgressReport(int progress, const char* status);
    void OnRoomParamsUpdated(std::function<void(const RoomParams&)> callback);
    void processAttrValue(std::string attr_name, int value);
    void deinit();
    void sendTraceLog(const char* level, const char* message);

    bool uploadP0Data(const void* data, size_t data_len);

    MqttClient() = default;
    ~MqttClient() = default;
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    void ReportTimer();
    static const char* kGizwitsProtocolJson;

private:
    Mqtt* mqtt_ = nullptr;
    std::function<void(const RoomParams&)> room_params_updated_callback_;
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
    int port_ = 1883;
    std::string client_id_;
    std::string username_;
    static int attr_size_;

    static void messageReceiveHandler(void* arg);
    static void messageResendHandler(void* arg);
    static void timerCallback(TimerHandle_t xTimer);
    bool parseRealtimeAgent(const char* in_str, int in_len, room_params_t* params);
    bool parseM2MCtrlMsg(const char* in_str, int in_len);
    void handleMqttMessage(mqtt_msg_t* msg);
    void app2devMsgHandler(const uint8_t *data, int32_t len);
    uint8_t mqttNumRemLenBytes(const uint8_t *buf);
};
