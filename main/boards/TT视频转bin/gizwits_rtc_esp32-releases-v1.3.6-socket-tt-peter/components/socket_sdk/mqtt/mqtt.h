#ifndef __MQTT__
#define __MQTT__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAGENT_PROTOCOL_VERSION     (0x00000003)
#define HI_CMD_PAYLOAD93            0x0093
#define HI_CMD_UPLOADACK94          0x0094
#define HI_CMD_MQTT_RESET           0x021E

// 定义MQTT配置数据结构
typedef struct {
    char product_key[33];      // 产品密钥
    char product_secret[33];   // 产品密码
    char mqtt_address[64];     // MQTT服务器地址
    char mqtt_port[16];        // MQTT服务器端口
} mqtt_config_t;

typedef struct {
    char *topic;    // 消息主题
    char *payload;  // 消息内容
    char *data;     // 原始数据
    int32_t data_len; // 原始数据长度
    int32_t payload_len;    // 消息内容长度
    int32_t topic_len;    // 消息内容长度
    int32_t qos;    // 消息服务等级
    int32_t mid;    // 消息id
} jl_mqtt_msg_t;

void mqtt_init(void *priv, mqtt_config_t *config);
int32_t mqtt_publish(char *features, char *payload, int32_t payload_len, int qos) ;
bool mqtt_report_rtc_room_info(bool event, char *reason);
bool mqtt_get_room_info(void);
int mqtt_get_published_id(void);
int mqtt_sendReset2Cloud( void );
int mqtt_sendGizProtocol2Cloud(const char *topic, uint8_t flag, uint16_t cmd, uint32_t sn, uint8_t *data, uint16_t len );
const char* getDev2AppTopic();
void mqtt_sem_give(void);
uint8_t get_mqtt_is_connected(void);
/**
 * @brief Send a log message to the MQTT broker
 * 
 * @param log The log message to send
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t send_trace_log(const char *log, const char *extra);
void set_need_switch_socket_room(uint8_t enable);
// void set_room_info_req_success(uint8_t success);
uint32_t __set_room_info_request_id(uint32_t id, const char* fun, int32_t line);
#define set_room_info_request_id(id) __set_room_info_request_id(id, __FUNCTION__, __LINE__)

#ifdef __cplusplus
}
#endif
#endif