#pragma once

#include <string>
#include <functional>
#include <mutex>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "cJSON.h"
#include "protocols/protocol.h"
#include <mqtt.h>

// 内存优化配置
// S3 用更大的内存
#if CONFIG_IDF_TARGET_ESP32S3
#define MQTT_TASK_STACK_SIZE_RCV     1024 * 6    // 消息接收任务栈大小 - 增加以处理大型JSON
#define MQTT_TASK_STACK_SIZE_RESEND  4096    // 消息重发任务栈大小
#else
#define MQTT_TASK_STACK_SIZE_RCV     4096    // 消息接收任务栈大小 - 增加以处理大型JSON
#define MQTT_TASK_STACK_SIZE_RESEND  2048    // 消息重发任务栈大小
#endif
#define MQTT_QUEUE_SIZE              5      // 消息队列大小
#define MQTT_TOPIC_BUFFER_SIZE       48      // 主题缓冲区大小
#define MQTT_PAYLOAD_BUFFER_SIZE     256     // 负载缓冲区大小
#define MQTT_TOKEN_REPORT_BUFFER_SIZE 128    // Token报告缓冲区大小
#define MQTT_SEND_QUEUE_SIZE        6     // 发送队列大小

// 协议常量（供app2dev解析等使用）
#define GAGENT_PROTOCOL_VERSION     (0x00000003)
#define HI_CMD_PAYLOAD93            0x0093
#define HI_CMD_UPLOADACK94          0x0094
#define HI_CMD_MQTT_RESET           0x021E
#define MQTT_REQUEST_FAILURE_COUNT  10

// 发送队列控制消息标识（使用qos字段传递特殊控制）
#define MQTT_SEND_CONTROL_ROOMINFO  (-1)
#define MQTT_SEND_CONTROL_TOKEN_REFRESH  (-2)


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

// Outgoing publish message structure
typedef struct {
    char* topic;
    char* payload;
    size_t payload_len; // 添加 payload 长度字段
    int qos; // reserved for QoS or control (e.g. MQTT_SEND_CONTROL_ROOMINFO)
} mqtt_send_msg_t;

// RTC parameters structure
typedef struct {
    char bot_id[64];
    char voice_id[64];
    char user_id[64];
    char conv_id[64];
    char access_token[256];
    char voice_lang[64];
    char api_domain[256];
    char config[1024];  // 新增：保存 coze_websocket.config 的 JSON 字符串，增加到 1KB
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
    bool connect();
    bool disconnect();
    bool publish(const std::string& topic, const std::string& payload);
    bool subscribe(const std::string& topic);
    void setMessageCallback(std::function<void(const std::string&, const std::string&)> callback);
    void sendTokenReport(int total, int output, int input);
    bool GetRoomInfo(bool is_active_request = true);
    int sendResetToCloud();
    int getPublishedId();
    void OnRoomParamsUpdated(std::function<void(const RoomParams&, bool is_mutual)> callback);
    void sendOtaProgressReport(int progress, const char* status);
    void deinit();
    void sendTraceLog(const char* level, const char* message);

    bool uploadP0Data(const void* data, size_t data_len);

    MqttClient() = default;
    ~MqttClient() = default;
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
    void ReportTimer();
    
    // 内存优化相关函数
    void printMemoryUsage();
    size_t getEstimatedMemoryUsage();

private:
    std::unique_ptr<Mqtt> mqtt_;
    std::function<void(const RoomParams&, bool is_mutual)> room_params_updated_callback_;
    std::function<void(const std::string&, const std::string&)> message_callback_;
    QueueHandle_t message_queue_ = nullptr;
    QueueHandle_t send_queue_ = nullptr; // 新增：发送消息队列
    SemaphoreHandle_t mqtt_sem_ = nullptr;
    TimerHandle_t timer_ = nullptr;
    TimerHandle_t token_refresh_timer_ = nullptr;  // Token 自动刷新定时器
    int mqtt_event_ = 0;
    int mqtt_request_failure_count_ = 0;
    int mqtt_published_id_ = -1;
    bool is_first_connect_ = true;
    std::string password_;
    std::string endpoint_;
    int port_ = 1883;
    std::string client_id_;
    std::string username_;
    bool is_active_request_ = false;  // 标记是否为主动请求
    static int attr_size_;
    int disconnect_error_count_ = 0;  // 断开连接错误计数器

    static void messageReceiveHandler(void* arg);
    static void sendTask(void* arg);
    void app2devMsgHandler(const uint8_t *data, int32_t len);
    static void timerCallback(TimerHandle_t xTimer);
    static void tokenRefreshTimerCallback(TimerHandle_t xTimer);  // Token 刷新定时器回调
    static void reconnectTask(void* arg);

    void processAttrValue(std::string attr_name, int value);
    uint8_t mqttNumRemLenBytes(const uint8_t *buf);
    bool parseRealtimeAgent(const char* in_str, int in_len, room_params_t* params);
    bool parseM2MCtrlMsg(const char* in_str, int in_len);
    void handleMqttMessage(mqtt_msg_t* msg);
    void startTokenRefreshTimer();  // 启动 token 刷新定时器
    void stopTokenRefreshTimer();   // 停止 token 刷新定时器
};
