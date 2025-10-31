/* MQTT (over TCP) Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "services/gsever.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt.h"
#include "cJSON.h"
#include "config.h"
#include "server_protocol/pack_server_protocol.h"
#include "server_protocol/parse_server_protocol.h"
#include "ota/ota.h"
#include "sdk_api/sdk_version.h"
#include "mqtt_protocol.h"
#include "gizwits_protocol.h"
#include "tt_ledc.h"
#include "hall_switch.h"


static const char *TAG = "mqtt_example";
static esp_mqtt_client_handle_t client;
static QueueHandle_t g_message_rcv_queue = NULL;
static void mqtt_message_rcv_handle_thread(void *priv);
void mqtt_reconnect_task(void *arg);
static bool get_realtime_agent_cb(char* in_str, int in_len, rtc_params_t* params);
static int mqtt_client_handle_t(esp_mqtt_client_handle_t *c, jl_mqtt_msg_t *msg);
static bool get_m2m_ctrl_msg_cb(char* in_str, int in_len);
static TimerHandle_t xTimer =  NULL;
SemaphoreHandle_t mqtt_sem = NULL;
static int mqtt_event = MQTT_EVENT_CONNECTED; // 网络状态
static int mqtt_request_failure_count = 0;
static int mqtt_published_id = -1;
static bool need_switch_socket_room = false;
static bool is_first_connect = true;
#define MQTT_REQUEST_FAILURE_COUNT 10
static void mqtt_message_reSend_handle_thread(void *priv);
static uint8_t ws_error_played = 0;
void set_ws_error_played(uint8_t enable)
{
    ws_error_played = enable;
}

void set_need_switch_socket_room(uint8_t enable)
{
    need_switch_socket_room = enable;
}

static mqtt_config_t *g_mqtt_config = NULL;

// 添加静态任务相关定义
static StackType_t *mqtt_rcv_task_stack = NULL;
static StaticTask_t mqtt_rcv_task_buffer;
static StackType_t *mqtt_resend_task_stack = NULL;
static StaticTask_t mqtt_resend_task_buffer;
static StackType_t *mqtt_app_task_stack = NULL;
static StaticTask_t mqtt_app_task_buffer;

static uint8_t mqtt_is_connected = false;
uint8_t get_mqtt_is_connected(void) {
    return mqtt_is_connected;
}

// Add this global variable to track if a reconnection is in progress

// 初始化任务堆栈
static bool init_mqtt_task_stacks(void) {
    // 分配接收任务的堆栈
    mqtt_rcv_task_stack = (StackType_t *)heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mqtt_rcv_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate mqtt receive task stack in PSRAM");
        return false;
    }

    // 分配重发任务的堆栈
    mqtt_resend_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mqtt_resend_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate mqtt resend task stack in PSRAM");
        heap_caps_free(mqtt_rcv_task_stack);
        mqtt_rcv_task_stack = NULL;
        return false;
    }

    return true;
}

// 清理函数
static void cleanup_mqtt_tasks(void) {
    if (mqtt_rcv_task_stack) {
        heap_caps_free(mqtt_rcv_task_stack);
        mqtt_rcv_task_stack = NULL;
    }
    if (mqtt_resend_task_stack) {
        heap_caps_free(mqtt_resend_task_stack);
        mqtt_resend_task_stack = NULL;
    }
    if (mqtt_app_task_stack) {
        heap_caps_free(mqtt_app_task_stack);
        mqtt_app_task_stack = NULL;
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGI(TAG, "Last error %s: 0x%x\n", message, error_code);
    }
}


/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGW(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "\n", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t _client = event->client;
    product_info_t *pInfo = get_product_info();
    char topic[64] = {0};
    int topic_len = 0;
    int msg_id;
    mqtt_event = event_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:

        topic_len = snprintf(topic, sizeof(topic) - 1, "llm/%s/config/response",pInfo->szDID);
        topic[topic_len] = '\0';
        msg_id = esp_mqtt_client_subscribe(_client, topic, 0);
        ESP_LOGI(TAG, "%s sent subscribe successful, msg_id=%d\n", topic,msg_id);
        topic_len = snprintf(topic, sizeof(topic) - 1, "llm/%s/config/push",pInfo->szDID);
        topic[topic_len] = '\0';
        msg_id = esp_mqtt_client_subscribe(_client, topic, 1);
        ESP_LOGI(TAG, "%s sent subscribe successful, msg_id=%d\n", topic,msg_id);

        // 订阅 服务端通知
        topic_len = snprintf(topic, sizeof(topic) - 1, "ser2cli_res/%s",pInfo->szDID);
        topic[topic_len] = '\0';
        msg_id = esp_mqtt_client_subscribe(_client, topic, 1);
        ESP_LOGI(TAG, "%s sent subscribe successful, msg_id=%d\n", topic,msg_id);

        // 订阅 app消息
        topic_len = snprintf(topic, sizeof(topic) - 1, "app2dev/%s/+",pInfo->szDID);
        topic[topic_len] = '\0';
        msg_id = esp_mqtt_client_subscribe(_client, topic, 1);
        ESP_LOGI(TAG, "%s sent subscribe successful, msg_id=%d\n", topic,msg_id);

        // 自动上报一次版本号
        send_version_report();
        if (is_first_connect) {
            printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
            xSemaphoreGive(mqtt_sem); // 释放信号量,请求智能体信息
            is_first_connect = false;
        }
        
        mqtt_is_connected = true;
        // tt_led_strip_set_state(TT_LED_STATE_WHITE);
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_is_connected = false;
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED\n");
        // tt_led_strip_set_state(TT_LED_STATE_ORANGE);
        // xTaskCreate(mqtt_reconnect_task, "mqtt_reconnect", 4096, NULL, 5, NULL);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d\n", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d\n", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d\n", event->msg_id);
        mqtt_published_id = event->msg_id;
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);

        // 发送事件处理
        jl_mqtt_msg_t jl_message = {0};
        jl_message.qos = event->qos;
        // 复制字符串做补结束符处理
        if(event->data_len)
        {
            jl_message.payload = MALLOC(event->data_len+1);
            if(!jl_message.payload)   return;
            strncpy(jl_message.payload, event->data, event->data_len);
            
            // 也拷贝原始数据
            jl_message.data = event->data;
            jl_message.data_len = event->data_len;
            *(jl_message.payload+event->data_len) = '\0';
        }
        else
        {
            jl_message.payload = NULL;
        }
        jl_message.payload_len = event->data_len;

        if(event->topic_len)
        {
            jl_message.topic = MALLOC(event->topic_len+1);
            if(!jl_message.topic)   return;
            strncpy(jl_message.topic, event->topic, event->topic_len);
            *(jl_message.topic+event->topic_len) = '\0';
        }
        else
        {
            jl_message.topic = NULL;
        }
        jl_message.topic_len = event->topic_len;

        int iRet = xQueueSendToBack(g_message_rcv_queue, &jl_message, portMAX_DELAY);   // todo check portMAX_DELAY?
        ESP_LOGI(TAG, "xQueueSendToBack ret=%d\n", iRet);

        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT_EVENT_ERROR\n");
        mqtt_is_connected = false;
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)\n", strerror(event->error_handle->esp_transport_sock_errno));
            
            // xTaskCreate(mqtt_reconnect_task, "mqtt_reconnect", 4096, NULL, 5, NULL);
        }
        break;
    default:
        // printf("Other event id:%d\n", event->event_id);
        break;
    }
}

void mqtt_reconnect_task(void *arg)
{
    ESP_LOGI(TAG, "开始 MQTT 重连任务");
    
    // 延迟一段时间确保系统稳定
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 检查是否有有效的配置
    if (g_mqtt_config == NULL) {
        ESP_LOGE(TAG, "MQTT 配置未设置，无法重连");
        vTaskDelete(NULL);
        return;
    }
    
    // 检查配置中的地址和端口是否有效
    if (strlen(g_mqtt_config->mqtt_address) == 0 || strlen(g_mqtt_config->mqtt_port) == 0) {
        ESP_LOGE(TAG, "MQTT 地址或端口无效，无法重连");
        ESP_LOGE(TAG, "地址: '%s', 端口: '%s'", 
                g_mqtt_config->mqtt_address, 
                g_mqtt_config->mqtt_port);
        vTaskDelete(NULL);
        return;
    }
    
    // 将旧的客户端句柄设为 NULL，但不尝试停止或销毁它
    client = NULL;
    
    // 直接启动新的 MQTT 连接
    ESP_LOGI(TAG, "直接启动新的 MQTT 连接");
    
    // 获取产品信息
    product_info_t *pInfo = get_product_info();
    if (pInfo == NULL) {
        ESP_LOGE(TAG, "获取产品信息失败");
        vTaskDelete(NULL);
        return;
    }
    
    // 准备 URL - 使用安全的字符串操作
    char url[128] = {0};
    int len = snprintf(url, sizeof(url) - 1, "mqtt://%s:%s", 
                      g_mqtt_config->mqtt_address, 
                      g_mqtt_config->mqtt_port);
    if (len < 0 || len >= sizeof(url) - 1) {
        ESP_LOGE(TAG, "URL 缓冲区溢出");
        vTaskDelete(NULL);
        return;
    }
    
    // 准备认证信息
    char userName[128] = {0};
    char szNonce[11] = {0};
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    
    int mlen = snprintf(userName, sizeof(userName) - 1, "%s|signmethod=sha256,signnonce=%s", 
                        pInfo->szDID, szNonce);
    if (mlen < 0 || mlen >= sizeof(userName) - 1) {
        ESP_LOGE(TAG, "用户名缓冲区溢出");
        vTaskDelete(NULL);
        return;
    }
    
    char *token = gatCreateToken(szNonce);
    if (token == NULL) {
        ESP_LOGE(TAG, "创建令牌失败");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "MQTT 重连参数:");
    ESP_LOGI(TAG, "  URL: %s", url);
    ESP_LOGI(TAG, "  用户名: %s", userName);
    ESP_LOGI(TAG, "  客户端 ID: %s", pInfo->szDID);
    
    // 配置 MQTT 客户端
    esp_mqtt_client_config_t mqtt_cfg = {0}; // 初始化为全零
    mqtt_cfg.broker.address.uri = url;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.credentials.client_id = pInfo->szDID;
    mqtt_cfg.credentials.username = userName;
    mqtt_cfg.credentials.authentication.password = token;
    mqtt_cfg.network.disable_auto_reconnect = true;
    
    // 初始化并启动客户端
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "初始化 MQTT 客户端失败");
        // xTaskCreate(mqtt_reconnect_task, "mqtt_reconnect", 4096, NULL, 5, NULL);

        vTaskDelete(NULL);
        // 重试
        return;
    }
    
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t start_result = esp_mqtt_client_start(client);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "启动 MQTT 客户端失败: %d", start_result);
        // xTaskCreate(mqtt_reconnect_task, "mqtt_reconnect", 4096, NULL, 5, NULL);
        vTaskDelete(NULL);
        // 重试
        return;
    }
    
    // 重置 MQTT 状态变量
    mqtt_event = MQTT_EVENT_CONNECTED;
    mqtt_request_failure_count = 0;
    mqtt_published_id = -1;
    // need_switch_socket_room = false;
    
    ESP_LOGI(TAG, "MQTT 客户端重连成功");
    
    // 如果需要播放网络错误提示音
    vTaskDelay(pdMS_TO_TICKS(1000));
    extern void play_network_error_with_debounce(void);
    play_network_error_with_debounce();
    
    vTaskDelete(NULL);
}

static void mqtt_app_start(void *arg)
{
    ESP_LOGI(TAG, "mqtt_app_start");
    if (!g_mqtt_config) {
        ESP_LOGE(TAG, "MQTT config not set");
        vTaskDelete(NULL);
        return;
    }

    // 打印完整的 MQTT 配置信息
    ESP_LOGI(TAG, "MQTT Configuration:");
    ESP_LOGI(TAG, "  Address: '%s'", g_mqtt_config->mqtt_address);
    ESP_LOGI(TAG, "  Port: '%s'", g_mqtt_config->mqtt_port);

    need_switch_socket_room = false;

    esp_log_level_set(TAG, ESP_LOG_INFO);  // 改为 INFO 级别以便于调试
    esp_log_level_set("mqtt_client", ESP_LOG_INFO);
    esp_log_level_set("mqtt_example", ESP_LOG_INFO);
    esp_log_level_set("transport_base", ESP_LOG_ERROR);
    esp_log_level_set("esp-tls", ESP_LOG_ERROR);
    esp_log_level_set("transport", ESP_LOG_ERROR);
    esp_log_level_set("outbox", ESP_LOG_ERROR);

    product_info_t *pInfo = get_product_info();

    uint8_t url[128] = {0};
    // 拼接URL前先检查数据有效性
    if (strlen(g_mqtt_config->mqtt_address) == 0 || strlen(g_mqtt_config->mqtt_port) == 0) {
        ESP_LOGE(TAG, "Invalid MQTT address or port");
        vTaskDelete(NULL);
        return;
    }

    // 拼接URL
    int len = snprintf((char *)url, sizeof(url), "mqtt://%s:%s", 
                      g_mqtt_config->mqtt_address, 
                      g_mqtt_config->mqtt_port);
    if (len < 0 || len >= sizeof(url)) {
        ESP_LOGE(TAG, "URL buffer overflow");
        vTaskDelete(NULL);
        return;
    }
    url[len] = '\0';
    ESP_LOGI(TAG, "MQTT URL: %s", url);

    uint8_t userName[128]= {0};
    uint8_t szNonce[11] = {0};
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    //内容拼接
    int mlen = snprintf((char *)userName, sizeof(userName) - 1, "%s|signmethod=sha256,signnonce=%s", pInfo->szDID,szNonce);
    userName[mlen] = '\0';
    char *token = gatCreateToken(szNonce);

    // 打印参数 
    ESP_LOGI(TAG, "url: %s", url);
    ESP_LOGI(TAG, "userName: %s", userName);
    ESP_LOGI(TAG, "token: %s", token);
    ESP_LOGI(TAG, "pInfo->szDID: %s", pInfo->szDID);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = url,
        .session = {
            .keepalive = 60,
        },
        .credentials = {
            .client_id = pInfo->szDID,
            .username = (const char *)userName,
            .authentication = {
                .password = token,
            },
        },
        // .network = {
        //     .disable_auto_reconnect = true,
        // },
    };

    // 创建新的 MQTT 客户端
    client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vTaskDelete(NULL);
        return;
    }
    
    // 注册事件处理函数
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    // 启动 MQTT 客户端
    esp_err_t start_result = esp_mqtt_client_start(client);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", start_result);
        vTaskDelete(NULL);
        return;
    }

    // 初始化任务堆栈
    if (!init_mqtt_task_stacks()) {
        ESP_LOGE(TAG, "Failed to initialize mqtt task stacks");
        vTaskDelete(NULL);
        return;
    }

    // 创建消息接收队列（如果不存在）
    if (g_message_rcv_queue == NULL) {
        g_message_rcv_queue = xQueueCreate(50, sizeof(jl_mqtt_msg_t));
        if (g_message_rcv_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create message queue");
            cleanup_mqtt_tasks();
            vTaskDelete(NULL);
            return;
        }
    }

    // 使用静态创建方式创建任务
    TaskHandle_t rcv_task = xTaskCreateStaticPinnedToCore(
        &mqtt_message_rcv_handle_thread,
        "mqtt_client",
        8 * 1024,
        NULL,
        5,
        mqtt_rcv_task_stack,
        &mqtt_rcv_task_buffer,
        0
    );

    TaskHandle_t resend_task = xTaskCreateStaticPinnedToCore(
        &mqtt_message_reSend_handle_thread,
        "mqtt_reSend",
        4 * 1024,
        NULL,
        5,
        mqtt_resend_task_stack,
        &mqtt_resend_task_buffer,
        0
    );

    if (!rcv_task || !resend_task) {
        ESP_LOGE(TAG, "Failed to create mqtt tasks");
        cleanup_mqtt_tasks();
        vTaskDelete(NULL);
        return;
    }

    vTaskDelete(NULL);
}

void mqtt_init(void *priv, mqtt_config_t *config)
{
    // 保存新配置
    if (config) {
        // if (g_mqtt_config) {
        //     free(g_mqtt_config);
        // }
        g_mqtt_config = (mqtt_config_t *)malloc(sizeof(mqtt_config_t));
        if (g_mqtt_config) {
            memcpy(g_mqtt_config, config, sizeof(mqtt_config_t));
        }
    }
    
    // 分配 PSRAM 堆栈
    mqtt_app_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mqtt_app_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate mqtt app task stack in PSRAM");
        // free(g_mqtt_config);
        // g_mqtt_config = NULL;
        return;
    }

    // 使用静态创建方式创建任务
    TaskHandle_t app_task = xTaskCreateStaticPinnedToCore(
        &mqtt_app_start,
        "mqtt_app_start",
        4 * 1024,
        NULL,
        5,
        mqtt_app_task_stack,
        &mqtt_app_task_buffer,
        0
    );

    if (!app_task) {
        ESP_LOGE(TAG, "Failed to create mqtt app task");
        heap_caps_free(mqtt_app_task_stack);
        mqtt_app_task_stack = NULL;
        return;
    }
}

static int32_t mqtt_publish_msg(char *features, char *payload, int32_t payload_len, int qos) 
{
    // 参数校验
    if (client == NULL) {
        // printf("MQTT client not initialized\n");
        return -1;
    }
    if (features == NULL) {
        // printf("Invalid features parameter\n");
        return -2;
    }
    if (payload == NULL || payload_len < 0) {
        // printf("Invalid payload parameter\n");
        return -3;
    }

    // 获取产品信息
    product_info_t *pInfo = get_product_info();
    if (pInfo == NULL) {
        // printf("Failed to get product info\n");
        return -4;
    }

    // 构建主题
    char topic[64];
    int topic_len = snprintf(topic, sizeof(topic), "llm/%s/config/%s", pInfo->szDID, features);

    // 打印 topic 和 payload
    // printf("Topic: %s\n", topic);
    // printf("Payload: %.*s\n", payload_len, payload); // 使用 %.*s 打印指定长度的字符串

    if (topic_len < 0 || topic_len >= sizeof(topic)) {
        // printf("Topic construction failed or truncated\n");
        return -1;
    }

    // 发布消息
    int msg_id = esp_mqtt_client_publish(client, topic, payload, payload_len, qos, 0);
    if (msg_id < 0) {
        // printf("Failed to publish message, error code: %d\n", msg_id);
        return -1;
    }

    // printf("Message published successfully, msg_id=%d, topic=%s\n", msg_id, topic);
    return msg_id;
}


// 上报版本号
void send_version_report(void) {
    send_trace_log("发送固件版本号", "");

    if (client == NULL) {
        // printf("MQTT client not initialized\n");
        return -1;
    }

    uint8_t buf[256] = {0};
    version_info_t ver_info = {
        .subdev_id = "",
        .module_sw_ver = sdk_get_software_version(),
        .module_hw_ver = sdk_get_hardware_version(),
    };

    int len = pack_version_report(buf, sizeof(buf), &ver_info);
    if (len > 0) {
        // 发布消息
        int msg_id = esp_mqtt_client_publish(client, "cli2ser_req", buf, len, 1, 0);
        if (msg_id < 0) {
            send_trace_log("发送固件版本号失败", "");
            return -1;
        }
        send_trace_log("发送固件版本号成功", "");
    }
}


static void mqtt_message_rcv_handle_thread(void *priv)
{
    int ret;
    jl_mqtt_msg_t msg = {0};
    g_message_rcv_queue = xQueueCreate(50, sizeof(jl_mqtt_msg_t));
    if(g_message_rcv_queue == NULL)
    {
        // printf("xQueueCreate g_message_rcv_queue failed! size=%u\n", 50*sizeof(jl_mqtt_msg_t));
        vTaskDelete(NULL);
    }

    while (1)
    {
        xQueueReceive(g_message_rcv_queue, &msg, portMAX_DELAY);
        mqtt_client_handle_t(client, &msg);
        vTaskDelay(10);
    }
}

static void mqtt_message_reSend_handle_thread(void *priv)
{
    mqtt_sem = xSemaphoreCreateBinary();
    TickType_t last_request_time = 0; // 记录上次请求的时间
    const TickType_t delay_time = pdMS_TO_TICKS(3000); // 3秒的延迟时间

    while (1)
    {
        ESP_LOGI("MQTT", "Waiting for semaphore...");
        while(xSemaphoreTake(mqtt_sem, pdMS_TO_TICKS(portMAX_DELAY)) != pdTRUE);

        // If the lid is open, do not request
        if(get_hall_state() == HALL_STATE_OFF)
        {
            ESP_LOGI("MQTT", "Lid is open, skipping request");
            continue;
        }

        if (++mqtt_request_failure_count > MQTT_REQUEST_FAILURE_COUNT)
        {
            ESP_LOGW("MQTT", "Request failure count exceeded limit, deleting timer");
            if(xTimer)
            {
                xTimerDelete(xTimer, 0);
                xTimer = NULL;
            }
            mqtt_request_failure_count = 0;
            // Play a voice message indicating request failure
        }
        else
        {
            ESP_LOGI("MQTT", "Checking message request status... fail cnt:%d", mqtt_request_failure_count);
            while(get_msg_req() || audio_tone_url_is_playing())
            {
                // If still requesting message, block and wait
                ESP_LOGI("MQTT", "Waiting for message request to finish");
                vTaskDelay(3000/portTICK_PERIOD_MS);
            }

            // Check the last request time, if within 3 seconds, do not process to avoid replaying the opening due to cloud response delay or multiple requests
            
            // TickType_t current_time = xTaskGetTickCount();
            // if ((current_time - last_request_time) < delay_time)
            // {
            //     ESP_LOGI("MQTT", "Request ignored due to 3-second rule");
            //     continue;
            // }

            ESP_LOGI("MQTT", "Fetching room information...");
            mqtt_get_room_info();
            // last_request_time = current_time; // Update last request time
            ESP_LOGI("MQTT", "Request completed, last request time updated");
        }
    }
}

// 处理 MQTT 数据
static int mqtt_client_handle_t(esp_mqtt_client_handle_t *c, jl_mqtt_msg_t *msg)
{
    // 参数校验
    if (c == NULL || msg == NULL) {
        // printf("Invalid parameters: client or message is NULL\n");
        return -1; // 返回错误码
    }

    printf("Received message on topic: %.*s\n", msg->topic_len, msg->topic, msg->payload);
#pragma message("zmz debug 123")

    // printf("strnstr(msg->topic, \"ser2cli_res\", msg->topic_len) result:%d\n", strnstr(msg->topic, "ser2cli_res", msg->topic_len));
    // printf("strnstr(msg->topic, \"push\", msg->topic_len)        result:%d\n", strnstr(msg->topic, "push", msg->topic_len));
    // printf("strnstr(msg->topic, \"response\", msg->topic_len)    result:%d\n", strnstr(msg->topic, "response", msg->topic_len));
    // printf("strnstr(msg->topic, \"app2dev\", msg->topic_len)     result:%d\n", strnstr(msg->topic, "app2dev", msg->topic_len));



    // 根据 topic 处理消息
    if (strnstr(msg->topic, "response", msg->topic_len)) {
        ESP_LOGI(TAG, "Processing RTC room number\n");
        rtc_params_t rtc_params = {0};
        if (get_realtime_agent_cb(msg->payload, msg->payload_len, &rtc_params)) {
            // 使用获取到的参数加入房间
            join_room(&rtc_params, need_switch_socket_room);
            need_switch_socket_room = true;
        }
    } else if (strnstr(msg->topic, "push", msg->topic_len)) {
        // 处理设备 RTC 信令指令
        // printf("Processing RTC signaling command\n");
        get_m2m_ctrl_msg_cb(msg->payload,msg->payload_len);
    } else if (strnstr(msg->topic, "ser2cli_res", msg->topic_len)) {
        printf("strnstr result:%d\n", strnstr(msg->topic, "ser2cli_res", msg->topic_len));
        // 处理服务端通知
        server_protocol_data_t result = server_protocol_parse_data(msg->data, msg->data_len);
        if (result.success) {
            // 判断类型
            if (result.cmd == CMD_VERSION_REPORT_RESP) {
                version_server_info_t ver_info = result.data.version_info;
                // 解析成功，输出解析结果
                run_start_ota_task(ver_info.module_sw_ver, ver_info.download_url);
            }
        }
    } else if (strnstr(msg->topic, "app2dev", msg->topic_len)) {
        // 处理服务端通知
        // hexdump("app2dev",msg->payload,msg->payload_len);
        hexdump("app2dev",msg->data,msg->data_len);
        app2dev_msg_handler(msg->data, msg->data_len);
    } else {
        ESP_LOGW(TAG, "Unknown topic: %.*s\n", msg->topic_len, msg->topic);
    }

    // 释放内存
    if (msg->topic) FREE(msg->topic);
    if (msg->payload) FREE(msg->payload);
    
    return 0;
}


void app2dev_msg_handler(uint8_t *data, int32_t len)
{
    if (len < 11) { // 确保数据长度至少为固定包头(4) + 可变长度(1) + Flag(1) + 命令字(2) + 包序号(4)
        ESP_LOGE(TAG, "Data length too short");
        return;
    }

    // 解析固定包头
    uint32_t fixed_header = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    if (fixed_header != GAGENT_PROTOCOL_VERSION) {
        ESP_LOGE(TAG, "Invalid fixed header");
        return;
    }

    // 解析可变长度
    uint8_t var_len = mqtt_num_rem_len_bytes(data + 4);
    if (var_len == 0 || var_len > len - 4) {
        ESP_LOGE(TAG, "Invalid variable length");
        return;
    }
    else {
        ESP_LOGI(TAG, "var_len: %d", var_len);
    }

    // 解析Flag
    uint8_t flag = data[4 + var_len];
    if (flag != 0x00) {
        ESP_LOGE(TAG, "Invalid flag : 0x%x", flag);
        // return;
    }

    // 解析命令字
    uint16_t command = (data[5 + var_len] << 8) | data[6 + var_len];
    if (command != HI_CMD_PAYLOAD93) {
        ESP_LOGE(TAG, "Invalid command");
        // todo 目前只处理93数据点业务
        return;
    }

    // 解析包序号
    uint32_t sn = (data[7 + var_len] << 24) | (data[8 + var_len] << 16) | (data[9 + var_len] << 8) | data[10 + var_len];
    ESP_LOGI(TAG, "sn: %d", sn);

    // 解析业务指令
    uint8_t *business_instruction = data + 11 + var_len;
    int business_instruction_len = len - (11 + var_len);
    if (business_instruction_len > 65535) {
        ESP_LOGE(TAG, "Business instruction too long");
        return;
    }

    // 只处理93业务指令

    gatAppData2Local(command, sn, business_instruction, business_instruction_len);

}

#define SIMULATOR_UPLOAD_SIZE      (1024)   // TODO 需要根据实际情况调整

void gatAppData2Local( uint16_t cmd, uint32_t sn, uint8_t *data, int32_t len )
{
    int32_t outLen=0;
    uint8_t *uploadBuf=NULL;

    uploadBuf = malloc( SIMULATOR_UPLOAD_SIZE );
    if( NULL==uploadBuf )
    {
        ESP_LOGE(TAG, "%s malloc uploadBuf size:%d filed.\n",__FUNCTION__,SIMULATOR_UPLOAD_SIZE );
        return ;
    }
    memset( uploadBuf,0,SIMULATOR_UPLOAD_SIZE );

    gizIssuedProcess( NULL, data, len, uploadBuf, &outLen );
    if (outLen >= 0) {
        // 94 ACK NEW DEV STATUS
        mqtt_sendGizProtocol2Cloud(getDev2AppTopic(), 0, ++cmd, sn, (uint8_t *)uploadBuf, outLen );
    }
    free( uploadBuf );
    ESP_LOGI(TAG, "%s exit \n",__FUNCTION__ );
    return ;
}
static uint32_t room_info_request_id = 0;
uint32_t get_room_info_request_id(void)
{
    return room_info_request_id;
}
uint32_t __set_room_info_request_id(uint32_t id, const char* fun, int32_t line)
{
    ESP_LOGI(TAG, "%s %d ,by [%s %d] \n",__FUNCTION__, id, fun, line);
    room_info_request_id = id;
}
static uint8_t room_info_req_success = 0;

uint8_t get_room_info_req_success(void)
{
    return room_info_req_success;
}

void set_room_info_req_success(uint8_t success)
{
    room_info_req_success = success;
}

// 解析roomid、userid、token
static bool get_realtime_agent_cb(char* in_str, int in_len, rtc_params_t* params)
{
    printf("in_str:%s\n", in_str);
    
    // 解析 JSON 字符串
    cJSON *root = cJSON_Parse(in_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Error parsing JSON!");
        return false;
    }

    bool success = false;
    
    // 提取 body 和 coze_realtime
    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (strcmp(method->valuestring, "websocket.auth.response") == 0) {
        // 提取 body 对象
        cJSON *body = cJSON_GetObjectItem(root, "body");
        if (body) {
            // 提取 coze_websocket 对象
            cJSON *coze_websocket = cJSON_GetObjectItem(body, "coze_websocket");
            if (coze_websocket) {
                // 提取 bot_id
                cJSON *bot_id = cJSON_GetObjectItem(coze_websocket, "bot_id");
                if (bot_id && bot_id->valuestring) {
                    strncpy(params->bot_id, bot_id->valuestring, sizeof(params->bot_id) - 1);
                }
                
                // 提取 voice_id
                cJSON *voice_id = cJSON_GetObjectItem(coze_websocket, "voice_id");
                if (voice_id && voice_id->valuestring) {
                    strncpy(params->voice_id, voice_id->valuestring, sizeof(params->voice_id) - 1);
                }
                
                // 提取 user_id
                cJSON *user_id = cJSON_GetObjectItem(coze_websocket, "user_id");
                if (user_id && user_id->valuestring) {
                    strncpy(params->user_id, user_id->valuestring, sizeof(params->user_id) - 1);
                }
                
                // 提取 conv_id
                cJSON *conv_id = cJSON_GetObjectItem(coze_websocket, "conv_id");
                if (conv_id && conv_id->valuestring) {
                    strncpy(params->conv_id, conv_id->valuestring, sizeof(params->conv_id) - 1);
                }
                
                // 提取 access_token
                cJSON *access_token = cJSON_GetObjectItem(coze_websocket, "access_token");
                if (access_token && access_token->valuestring) {
                    strncpy(params->access_token, access_token->valuestring, sizeof(params->access_token) - 1);
                }

                // 提取 config
                cJSON *config = cJSON_GetObjectItem(coze_websocket, "config");
                if (config) {
                    // 创建 config 的深拷贝
                    params->config = cJSON_Duplicate(config, 1);
                    if (!params->config) {
                        ESP_LOGE(TAG, "Failed to duplicate config JSON");
                        cJSON_Delete(root);
                        if(!ws_error_played)
                        {
                            ws_error_played = 1;
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            audio_tone_play(1, 0, "spiffs://spiffs/network_error_need_reset.mp3");
                            // vTaskDelay(pdMS_TO_TICKS(2000));
                            ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
                        }
                        return false;
                    }
                }
                // 提取 workflow_id
                cJSON *workflow_id = cJSON_GetObjectItem(coze_websocket, "workflow_id");
                if (workflow_id && workflow_id->valuestring) {
                    strncpy(params->workflow_id, workflow_id->valuestring, sizeof(params->workflow_id) - 1);
                    params->workflow_id[sizeof(params->workflow_id) - 1] = '\0'; // 确保字符串以空字符结尾
                }


                // 如果所有必需的参数都存在，则设置成功标志
                if (params->bot_id[0] && params->voice_id[0] && 
                    params->user_id[0] && params->conv_id[0] && 
                    params->access_token[0]) {
                    success = true;
                    ESP_LOGI(TAG, "WebSocket params parsed successfully:");
                    ESP_LOGI(TAG, "  bot_id: %s", params->bot_id);
                    ESP_LOGI(TAG, "  voice_id: %s", params->voice_id);
                    ESP_LOGI(TAG, "  user_id: %s", params->user_id);
                    ESP_LOGI(TAG, "  conv_id: %s", params->conv_id);
                    ESP_LOGI(TAG, "  access_token: %s", params->access_token);
                    // ESP_LOGI(TAG, "  expires_in: %d", params->expires_in);
                }
            }
        }
    }

    // 释放 JSON 对象
    cJSON_Delete(root);

    // 如果获取参数失败，清空结构体
    if (!success) {
        memset(params, 0, sizeof(rtc_params_t));
        ESP_LOGE(TAG, "Failed to get all required RTC parameters");
        if(!ws_error_played)
        {
            ws_error_played = 1;
            audio_tone_play(1, 0, "spiffs://spiffs/network_error_need_reset.mp3");
        }
    }
    else
    {
        ws_error_played = 0;
    }

    // 删除定时器
    if (xTimer != NULL && xTimerDelete(xTimer, 0) == pdPASS) {
        ESP_LOGI(TAG, "Timer deleted successfully");
        xTimer = NULL;
    }

    return success;
}

// 解析m2m对RTC的控制
static bool get_m2m_ctrl_msg_cb(char *in_str, int in_len)
{
    // 打印输入字符串
    // printf("in_str: %s\n", in_str);

    // 解析 JSON 字符串
    cJSON *json = cJSON_Parse(in_str);
    if (json == NULL) {
        // printf("Error: Failed to parse JSON string.\n");
        return false; // 返回 false 表示解析失败
    }

    // 提取 "method" 字段
    cJSON *method = cJSON_GetObjectItem(json, "method");
    if (method == NULL || !cJSON_IsString(method)) {
        // printf("Error: 'method' field not found or is not a string.\n");
        cJSON_Delete(json);
        return false; // 返回 false 表示字段无效
    }

    // 打印 "method" 的值
    // printf("Method: %s\n", method->valuestring);

    // 根据 method 的值执行相应操作
    if (strcmp(method->valuestring, "rtc.room.join") == 0) {
        leave_room();
        printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
        xSemaphoreGive(mqtt_sem); // 释放信号量
        

    } else if (strcmp(method->valuestring, "rtc.room.leave") == 0) {
        // 调用 RTC 退出房间接口
        leave_room();
    } else if (strcmp(method->valuestring, "websocket.config.change") == 0) {
        // printf("Unknown method: %s\n", method->valuestring);
        // mqtt_get_room_info();
        printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
        xSemaphoreGive(mqtt_sem); // 释放信号量
    }

    // 释放 JSON 对象
    cJSON_Delete(json);

    return true; // 返回 true 表示解析成功
}

// 定时器回调函数
void TimerCallback(TimerHandle_t xTimer) {
    // printf("Timer expired!\n");
    printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
    xSemaphoreGive(mqtt_sem); // 释放信号量
}

void mqtt_report_token(int total, int output, int input) {
    // 分配足够的缓冲区来存储格式化后的消息
    char msg[256];
    
    // 格式化消息
    int len = snprintf(msg, sizeof(msg),
        "{\r\n"
        "    \"method\": \"token.report\",\r\n"
        "    \"body\": {\r\n"
        "        \"total\": %d,\r\n"
        "        \"output\": %d,\r\n"
        "        \"input\": %d\r\n"
        "    }\r\n"
        "}", total, output, input);

    if (len < 0 || len >= sizeof(msg)) {
        ESP_LOGE(TAG, "Error formatting message or buffer too small");
        return;
    }

    // 发布 MQTT 消息
    int result = mqtt_publish_msg("report", msg, len, 1);
    if (result < 0) {
        ESP_LOGE(TAG, "Failed to publish MQTT message, error code: %d", result);
        return;
    }

    ESP_LOGI(TAG, "Token report sent successfully: %s", msg);
}

bool mqtt_get_room_info(void)
{
    // 构造 JSON 消息
    send_trace_log("发送获取房间信息", "");

    const char *fmt = 
        "{\r\n"
        "    \"method\": \"websocket.auth.request\",\r\n"
        "    \"sn\": \"%d\"\r\n"
        "}";
    char msg[256];
    sprintf(msg, fmt, get_room_info_request_id());

    // 发布 MQTT 消息
    ESP_LOGI(TAG, "publish msg: %s", msg);
    int result = mqtt_publish_msg("request", msg, strlen(msg), 1);
    if (result<0) {
        send_trace_log("发送获取房间信息失败", "");
        printf("Failed to publish MQTT message");
        return false;
    }

    send_trace_log("发送获取房间信息成功", "");
    // printf("MQTT message published successfully\n");

    if (xTimer != NULL)
    {
        if (xTimerDelete(xTimer, 0) == pdPASS) {
            // printf("Timer deleted successfully!\n");
            xTimer = NULL;
        } else {
            // printf("Failed to delete timer!\n");
        }
    }

    xTimer = xTimerCreate("SingleShotTimer", pdMS_TO_TICKS(5000), pdFALSE, (void *)0, TimerCallback);// 单次启动

    // 启动定时器
    if (xTimerStart(xTimer, 0) != pdPASS) {
        // printf("Failed to start timer!\n");
    } else {
        // printf("start timer!\n");
    }

    return true;
}


#define htonl(n)  (((n & 0xff) << 24) |((n & 0xff00) << 8) |((n & 0xff0000UL) >> 8) |((n & 0xff000000UL) >> 24))
#define htons(n)  ((n & 0xff) << 8) | ((n & 0xff00) >> 8)


int mqtt_sendReset2Cloud( void ) 
{
    send_trace_log("发送重置消息", "");
    if (client == NULL) {
        return -2;
    }
    
    if (mqtt_event == MQTT_EVENT_DISCONNECTED) {
        // printf("mqtt_event: %d", mqtt_event);
        return -2;
    }
    
    // todo add sub data field
    uint8_t buf[4+2+2], pos = 0;

    uint32_t tmp_u32 = htonl(GAGENT_PROTOCOL_VERSION);
    memcpy(buf+pos, &tmp_u32, sizeof(tmp_u32));
    pos += sizeof(tmp_u32);

    uint16_t tmp_u16 = htons(HI_CMD_MQTT_RESET);
    memcpy(buf+pos, &tmp_u16, sizeof(tmp_u16));
    pos += sizeof(tmp_u16);

    // todo deal sub data
    uint16_t subDataLen = 0;
    tmp_u16 = htons(subDataLen);
    memcpy(buf+pos, &tmp_u16, sizeof(tmp_u16));
    pos += sizeof(tmp_u16);

    hexdump("cli2ser_req",buf,pos);

    // 发布消息
    int msg_id = esp_mqtt_client_publish(client, "cli2ser_req", buf, sizeof(buf), 1, 0);
    if (msg_id < 0) {
        send_trace_log("发送重置消息失败", "");
        // printf("Failed to publish message, error code: %d\n", msg_id);
        return -1;
    }

    send_trace_log("发送重置消息成功", "");
    return msg_id;
}



const char* getDev2AppTopic()
{
    const char *format = "dev2app/%s";
    static char topic[64];

    if( get_product_info()->szDID[0] == '\0' )
    {
        return NULL;
    }

    if(topic[0] == '\0')
    {
        snprintf(topic, sizeof(topic), format, get_product_info()->szDID);
    }

    return topic;
}

int mqtt_sendGizProtocol2Cloud(const char *topic, uint8_t flag, uint16_t cmd, uint32_t sn, uint8_t *data, uint16_t len ) 
{
    send_trace_log("发送93payload消息", "");
    if (client == NULL) {
        return -2;
    }
    
    if (mqtt_event == MQTT_EVENT_DISCONNECTED) {
        // printf("mqtt_event: %d", mqtt_event);
        return -2;
    }

    uint8_t *pGizBuf = NULL;
    int8_t val_headerLen=0;
    uint32_t gizDataLen = 0;
    uint8_t var_header[4] ={0};
    uint32_t pos=0;
    uint16_t netCmd = 0;
    uint32_t netSN =0;
    uint32_t lanPacketHead =htonl(GAGENT_PROTOCOL_VERSION);

    // ESP_LOGI(TAG, "%s %d\n",__FUNCTION__,__LINE__ );
    if( HI_CMD_UPLOADACK94==cmd || HI_CMD_PAYLOAD93==cmd)
    {
         /* 4(00000003)+varLen(len)+1(flag)+2(cmd)+sn(4B)+payload */
        val_headerLen = mqtt_len_of_bytes( len+1+2+4,var_header );
        gizDataLen = 4+val_headerLen+1+2+4+len;
        // 暂不考虑带DID flag
    }
    else {
        // todo 其他怎么算按协议划分，某些可能不需要SN
        val_headerLen = mqtt_len_of_bytes( len+1+2+4,var_header );
        gizDataLen = 4+val_headerLen+1+2+4+len;
    }

    pGizBuf = (uint8_t*)malloc( gizDataLen );
    if( NULL==pGizBuf )
    {
        ESP_LOGE(TAG, "%s malloc size:%d error\n",__FUNCTION__,gizDataLen );
        return NULL;
    }
    memset( pGizBuf,0,gizDataLen );

    memcpy( pGizBuf+pos,&lanPacketHead,sizeof(lanPacketHead) );
    pos+=sizeof(lanPacketHead);
    memcpy( pGizBuf+pos,var_header,val_headerLen );
    pos+=val_headerLen;

    pGizBuf[pos] = flag;
    pos++;

    netCmd = htons(cmd);
    memcpy( pGizBuf+pos,&netCmd,2 );
    pos+=2;

    if( HI_CMD_UPLOADACK94==cmd || HI_CMD_PAYLOAD93==cmd)
    {
        netSN = htonl( sn );
        memcpy( pGizBuf+pos,&netSN,4 );
        pos+=4;
        ESP_LOGI(TAG, "%s %d pos %d\n",__FUNCTION__,__LINE__ , pos);
    }

    if( len )
    {
        memcpy( pGizBuf+pos,data,len );
        pos+=len;
    }

    hexdump("mqtt_sendGizProtocol2Cloud",pGizBuf,pos);

    // 发布消息
    int msg_id = esp_mqtt_client_publish(client, topic, pGizBuf, gizDataLen, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "%s Failed to publish message, error code: %d\n", topic, msg_id);
        // printf("Failed to publish message, error code: %d\n", msg_id);
    }

    if(pGizBuf) {
        free(pGizBuf);
    }

    // send_trace_log("发送重置消息成功", "");
    return gizDataLen;
}


int mqtt_get_published_id(void)
{
    return mqtt_published_id;
}

// 在程序退出时清理资源
void mqtt_deinit(void) {
    if (g_mqtt_config) {
        free(g_mqtt_config);
        g_mqtt_config = NULL;
    }
    cleanup_mqtt_tasks();
}

/**
 * @brief Send a log message to the MQTT broker
 * 
 * This function formats a log message and publishes it to the MQTT broker
 * on a topic specific to this device.
 * 
 * @param log The log message to send
 * @return ESP_OK if successful, otherwise an error code
 */
esp_err_t send_trace_log(const char *log, const char *extra) {
    // todo peter debug  mark
    return 0 ;
    if (client) {
        char* trace_id = get_trace_id();

        product_info_t *pInfo = get_product_info();
        // Format the topic
        char topic[64] = {0};
        snprintf(topic, sizeof(topic), "sys/%s/log", pInfo->szDID);
        
        // Format the payload
        char payload[512] = {0};
        snprintf(payload, sizeof(payload), "{\"message\": \"%s\", \"trace_id\": \"%s\", \"extra\": \"%s\"}", 
                log, trace_id, extra); // Add timestamp in milliseconds
        
        // Publish the message with QoS 1 to ensure delivery
        int msg_id = esp_mqtt_client_publish(client, topic, payload, strlen(payload), 0, 0);
        
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Failed to publish log message");
            return ESP_FAIL;
        }
        
        ESP_LOGD(TAG, "Published log message, ID: %d", msg_id);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "MQTT client not initialized");
    return ESP_FAIL;
}

// 供外部调用，释放信号量
void mqtt_sem_give(void)
{
    if(mqtt_sem)
    {
        // printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
        xSemaphoreGive(mqtt_sem);
    }
}