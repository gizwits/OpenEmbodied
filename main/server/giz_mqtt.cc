#include "giz_mqtt.h"
#include "giz_api.h"
#include "board.h"
#include <esp_log.h>
#include <ml307_mqtt.h>
#include "protocol/iot_protocol.h"
#include "protocol/ota_protocol.h"
#include <cstring>
#include "auth.h"
#include <arpa/inet.h>
#include "application.h"
#include "settings.h"
#include <esp_wifi.h>
#include <esp_app_desc.h>
#include "ntp.h"

#define TAG "GIZ_MQTT"

int MqttClient::attr_size_ = 0;

void MqttClient::InitAttrsFromJson() {
    cJSON* root = cJSON_Parse(MqttClient::kGizwitsProtocolJson);
    if (!root) return;
    cJSON* entities = cJSON_GetObjectItem(root, "entities");
    if (!entities || !cJSON_IsArray(entities)) { cJSON_Delete(root); return; }
    cJSON* entity0 = cJSON_GetArrayItem(entities, 0);
    if (!entity0) { cJSON_Delete(root); return; }
    cJSON* attrs = cJSON_GetObjectItem(entity0, "attrs");
    if (!attrs || !cJSON_IsArray(attrs)) { cJSON_Delete(root); return; }
    int attr_count = cJSON_GetArraySize(attrs);
    for (int i = 0; i < attr_count; ++i) {
        cJSON* attr = cJSON_GetArrayItem(attrs, i);
        if (!attr) continue;
        cJSON* name = cJSON_GetObjectItem(attr, "name");
        cJSON* position = cJSON_GetObjectItem(attr, "position");
        if (!name || !position) continue;
        cJSON* byte_offset = cJSON_GetObjectItem(position, "byte_offset");
        cJSON* bit_offset = cJSON_GetObjectItem(position, "bit_offset");
        cJSON* len = cJSON_GetObjectItem(position, "len");
        cJSON* unit = cJSON_GetObjectItem(position, "unit");
        if (!byte_offset || !bit_offset || !len || !unit) continue;
        Attr a;
        a.name = name->valuestring;
        a.byte_offset = byte_offset->valueint;
        a.bit_offset = bit_offset->valueint;
        a.len = len->valueint;
        a.unit = unit->valuestring;
        g_attrs.push_back(a);
    }
    attr_size_ = (attr_count + 8 - 1) / 8;
    cJSON_Delete(root);
}

// MqttClient implementation
bool MqttClient::initialize() {
    
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        delete mqtt_;
    }

    Settings settings("wifi", true);
    bool need_activation = settings.GetInt("need_activation");
    // 创建信号量用于等待回调完成
    SemaphoreHandle_t config_sem = xSemaphoreCreateBinary();
    if (config_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }

    /**
    这里有两种情况，如果authkey 为空的情况下，使用临时授权
     */
    client_id_ = Auth::getInstance().getDeviceId();
    bool has_authkey = !Auth::getInstance().getAuthKey().empty();

    if (!has_authkey) {
        if (need_activation == 1) {
            GServer::activationLimitDevice([this, config_sem, &settings](mqtt_config_t* config) {
                endpoint_ = config->mqtt_address;
                port_ = std::stoi(config->mqtt_port);
                client_id_ = config->device_id;
                ESP_LOGI(TAG, "MQTT endpoint: %s, port: %d", endpoint_.c_str(), port_);
                // 保存did
                ESP_LOGI(TAG, "Device ID: %s", config->device_id);
                settings.SetString("did", config->device_id);
                xSemaphoreGive(config_sem);
                settings.SetInt("need_activation", 0);
            });
            
        } else {
            GServer::getLimitProvision([this, config_sem](mqtt_config_t* config) {
                endpoint_ = config->mqtt_address;
                port_ = std::stoi(config->mqtt_port);
                ESP_LOGI(TAG, "MQTT endpoint: %s, port: %d", endpoint_.c_str(), port_);
                xSemaphoreGive(config_sem);
            });
        }
        
    } else {
        if (need_activation == 1) {
            ESP_LOGI(TAG, "need_activation is true");
            // 调用注册
            GServer::activationDevice([this, config_sem, &settings](mqtt_config_t* config) {
                endpoint_ = config->mqtt_address;
                port_ = std::stoi(config->mqtt_port);
                ESP_LOGI(TAG, "MQTT endpoint: %s, port: %d", endpoint_.c_str(), port_);
                xSemaphoreGive(config_sem);

                settings.SetInt("need_activation", 0);
            });
        } else {
            ESP_LOGI(TAG, "need_activation is false");
            // 调用Provision 获取相关信息
            GServer::getProvision([this, config_sem](mqtt_config_t* config) {
                endpoint_ = config->mqtt_address;
                port_ = std::stoi(config->mqtt_port);
                ESP_LOGI(TAG, "MQTT endpoint: %s, port: %d", endpoint_.c_str(), port_);
                xSemaphoreGive(config_sem);
            });
        }
    }

    // 等待回调完成，超时时间设为10秒
    if (xSemaphoreTake(config_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for MQTT config");
        vSemaphoreDelete(config_sem);
        return false;
    }
    vSemaphoreDelete(config_sem);

    settings.SetInt("need_activation", 0);
    
    // 准备认证信息
    char userName[128] = {0};
    uint8_t szNonce[11] = {0};
    GServer::gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    int mlen = snprintf(userName, sizeof(userName) - 1, "%s|signmethod=sha256,signnonce=%s", 
                        client_id_.c_str(), szNonce);
    if (mlen < 0 || mlen >= sizeof(userName) - 1) {
        ESP_LOGE(TAG, "用户名缓冲区溢出");
        return false;
    }
    const char *token;
    
    if (has_authkey) {
        token = GServer::gatCreateToken(szNonce);
    } else {
        token = GServer::gatCreateLimitToken(szNonce);
    }
    if (token == NULL) {
        ESP_LOGE(TAG, "创建令牌失败");
        return false;
    }

    username_ = userName;
    password_ = token;
    // 不需要释放 token，因为它是静态分配的

    if (endpoint_.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        return false;
    }

    mqtt_ = Board::GetInstance().CreateMqtt();
    mqtt_->SetKeepAlive(20);

    mqtt_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Disconnected from endpoint");
        mqtt_event_ = 0;
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "OnMessage received - topic: %s, payload length: %zu", topic.c_str(), payload.length());
        
        if (message_callback_) {
            message_callback_(topic, payload);
        }

        // ESP_LOGI(TAG, "OnMessage topic: %s, payload: %s", topic.c_str(), payload.c_str());
        
        // 修复：恢复使用strdup确保字符串在消息处理期间保持有效
        mqtt_msg_t msg = {0};
        msg.topic = strdup(topic.c_str());
        msg.topic_len = topic.length();
        msg.payload = strdup(payload.c_str());
        msg.payload_len = payload.length();
        msg.data = static_cast<char*>(malloc(payload.size()));
        if (msg.data) {
            memcpy(msg.data, payload.data(), payload.size());
        }
        msg.data_len = payload.size();
        
        ESP_LOGI(TAG, "Message queued - topic: %s, payload: %s", msg.topic, msg.payload);
        
        if (message_queue_) {
            xQueueSendToBack(message_queue_, &msg, portMAX_DELAY);
        }
    });

    ESP_LOGI(TAG, "MQTT 连接参数:");
    ESP_LOGI(TAG, "  URL: %s", endpoint_.c_str());
    ESP_LOGI(TAG, "  用户名: %s", username_.c_str());
    ESP_LOGI(TAG, "  客户端 ID: %s", client_id_.c_str());
    ESP_LOGI(TAG, "  token: %s", password_.c_str());

    ESP_LOGI(TAG, "Connecting to endpoint %s:%d", endpoint_.c_str(), port_);
    if (!mqtt_->Connect(endpoint_, port_, client_id_, username_, password_)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        return false;
    }

    // Create message queue
    message_queue_ = xQueueCreate(MQTT_QUEUE_SIZE, sizeof(mqtt_msg_t));
    if (!message_queue_) {
        ESP_LOGE(TAG, "Failed to create message queue");
        return false;
    }

    // Create semaphore
    mqtt_sem_ = xSemaphoreCreateBinary();
    if (!mqtt_sem_) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        vQueueDelete(message_queue_);
        message_queue_ = nullptr;
        return false;
    }

    // Create tasks
    xTaskCreate(messageReceiveHandler, "mqtt_rcv", MQTT_TASK_STACK_SIZE_RCV, this, 5, nullptr);
    xTaskCreate(messageResendHandler, "mqtt_resend", MQTT_TASK_STACK_SIZE_RESEND, this, 5, nullptr);

    ESP_LOGI(TAG, "Connected to endpoint");
    // 订阅配置响应和推送
    std::string response_topic = "llm/" + client_id_ + "/config/response";
    std::string push_topic = "llm/" + client_id_ + "/config/push";
    std::string server_notify_topic = "ser2cli_res/" + client_id_;
    std::string p0_notify_topic = "app2dev/" + client_id_ + "/+";
    
    ESP_LOGI(TAG, "Subscribing to topics:");
    ESP_LOGI(TAG, "  %s (QoS 0)", response_topic.c_str());
    ESP_LOGI(TAG, "  %s (QoS 1)", push_topic.c_str());
    ESP_LOGI(TAG, "  %s (QoS 1)", server_notify_topic.c_str());
    ESP_LOGI(TAG, "  %s (QoS 0)", p0_notify_topic.c_str());
    
    vTaskDelay(pdMS_TO_TICKS(10));
    if (mqtt_->Subscribe(response_topic, 0) != 0) {
        ESP_LOGE(TAG, "Failed to subscribe to response topic");
    }
    
    if (mqtt_->Subscribe(push_topic, 1) != 0) {
        ESP_LOGE(TAG, "Failed to subscribe to push topic");
    }
    
    if (mqtt_->Subscribe(server_notify_topic, 1) != 0) {
        ESP_LOGE(TAG, "Failed to subscribe to server notify topic");
    }
    if (mqtt_->Subscribe(p0_notify_topic, 0) != 0) {
        ESP_LOGE(TAG, "Failed to subscribe to p0 notify topic");
        sendTraceLog("error", "订阅 p0 通知 失败");
    }
    
    
    // 获取房间信息
    getRoomInfo();
    mqtt_event_ = 1;
    
    // 打印内存使用情况
    printMemoryUsage();
    
    return true;
}

bool MqttClient::publish(const std::string& topic, const std::string& payload) {
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return false;
    }
    // ESP_LOGI(TAG, "publish topic: %s, payload: %s", topic.c_str(), payload.c_str());
    return mqtt_->Publish(topic, payload);
}

bool MqttClient::subscribe(const std::string& topic) {
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return false;
    }
    return mqtt_->Subscribe(topic);
}

void MqttClient::setMessageCallback(std::function<void(const std::string&, const std::string&)> callback) {
    message_callback_ = callback;
}


void MqttClient::sendTokenReport(int total, int output, int input) {
    char msg[MQTT_TOKEN_REPORT_BUFFER_SIZE];  // 优化：减少缓冲区大小
    int len = snprintf(msg, sizeof(msg),
        "{\"method\":\"token.report\",\"body\":{\"total\":%d,\"output\":%d,\"input\":%d}}", 
        total, output, input);

    if (len > 0 && len < sizeof(msg)) {
        publish("report", std::string(msg, len));
    }
}

void MqttClient::OnRoomParamsUpdated(std::function<void(const RoomParams&)> callback) {
    room_params_updated_callback_ = callback;
}

bool MqttClient::getRoomInfo() {
    const char* msg = "{\"method\":\"websocket.auth.request\"}";  // 优化：简化JSON格式

    if (!publish("llm/" + client_id_ + "/config/request", msg)) {
        return false;
    }

    if (timer_) {
        xTimerDelete(timer_, 0);
    }

    timer_ = xTimerCreate("SingleShotTimer", pdMS_TO_TICKS(5000), pdFALSE, this, timerCallback);
    if (timer_ && xTimerStart(timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start timer");
        return false;
    }

    return true;
}

int MqttClient::sendResetToCloud() {
    if (mqtt_ == nullptr || mqtt_event_ == 0) {
        return -2;
    }

    uint8_t buf[8] = {0};
    uint32_t version = htonl(0x00000003);
    uint16_t cmd = htons(0x021E);
    uint16_t subDataLen = htons(0);

    memcpy(buf, &version, 4);
    memcpy(buf + 4, &cmd, 2);
    memcpy(buf + 6, &subDataLen, 2);

    return publish("cli2ser_req", std::string((char*)buf, sizeof(buf))) ? 0 : -1;
}

int MqttClient::getPublishedId() {
    return mqtt_published_id_;
}


void MqttClient::sendTraceLog(const char* level, const char* message) {
    // ESP_LOGI(TAG, "sendTraceLog: %s", message);
    
    // Format the topic - 优化：减少缓冲区大小
    char topic[MQTT_TOPIC_BUFFER_SIZE] = {0};
    snprintf(topic, sizeof(topic), "sys/%s/log", client_id_.c_str());
    
    // Get current time using NTP client if available, otherwise use system time
    std::string time_str;
    auto& ntp_client = NtpClient::GetInstance();
    if (ntp_client.IsSynced()) {
        time_str = ntp_client.GetFormattedTime();
    } else {
        // Fallback to system time if NTP not synced
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        char sys_time_str[32];
        strftime(sys_time_str, sizeof(sys_time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
        time_str = std::string(sys_time_str);
    }
    
    // Format the payload - 优化：减少缓冲区大小，增加时间字段
    char payload[MQTT_PAYLOAD_BUFFER_SIZE] = {0};
    snprintf(payload, sizeof(payload), 
        "{\"message\":\"%s\",\"trace_id\":\"%s\",\"extra\":\"%s\",\"time\":\"%s\"}", 
        message, Application::GetInstance().GetTraceId(), level, time_str.c_str());

    // Publish the message
    if (!publish(topic, std::string(payload))) {
        ESP_LOGE(TAG, "Failed to publish log message");
    }
}

void MqttClient::sendOtaProgressReport(int progress, const char* status) {
    uint8_t buf[256] = {0};
    
    // 获取当前版本信息
    auto app_desc = esp_app_get_description();
    std::string sw_version = app_desc->version;
    std::string hw_version = BOARD_NAME;
    
    // 使用协议函数打包消息
    size_t total_len = ota::protocol::pack_mqtt_upgrade_progress(
        0x0002,  // MCU upgrade flag
        progress,
        hw_version.c_str(),
        sw_version.c_str(),
        status,
        buf,
        sizeof(buf)
    );

    if (total_len == 0) {
        ESP_LOGE(TAG, "Failed to pack OTA progress message");
        return;
    }

    // Log the hex dump of the message
    char hex_str[512] = {0};
    int pos = 0;
    for (size_t i = 0; i < total_len; i++) {
        pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X", buf[i]);
    }
    ESP_LOGI(TAG, "OTA Progress Report (hex): %s (len: %zu)", hex_str, total_len);

    // Publish the message
    if (!publish("cli2ser_req", std::string((char*)buf, total_len))) {
        ESP_LOGE(TAG, "Failed to publish OTA progress report");
    }
}

void MqttClient::deinit() {
    if (timer_) {
        xTimerDelete(timer_, 0);
        timer_ = nullptr;
    }

    if (mqtt_sem_) {
        vSemaphoreDelete(mqtt_sem_);
        mqtt_sem_ = nullptr;
    }

    if (message_queue_) {
        vQueueDelete(message_queue_);
        message_queue_ = nullptr;
    }

    if (mqtt_) {
        delete mqtt_;
        mqtt_ = nullptr;
    }
}

void MqttClient::messageReceiveHandler(void* arg) {
    MqttClient* client = static_cast<MqttClient*>(arg);
    mqtt_msg_t msg;

    while (1) {
        if (xQueueReceive(client->message_queue_, &msg, portMAX_DELAY) == pdTRUE) {
            client->handleMqttMessage(&msg);
            // 恢复内存释放
            free(msg.topic);
            free(msg.payload);
            free(msg.data);
        }
        vTaskDelay(10);
    }
}

void MqttClient::messageResendHandler(void* arg) {
    MqttClient* client = static_cast<MqttClient*>(arg);

    while (1) {
        if (xSemaphoreTake(client->mqtt_sem_, portMAX_DELAY) == pdTRUE) {
            if (++client->mqtt_request_failure_count_ > MQTT_REQUEST_FAILURE_COUNT) {
                if (client->timer_) {
                    xTimerDelete(client->timer_, 0);
                    client->timer_ = nullptr;
                }
            } else {
                client->getRoomInfo();
            }
        }
    }
}

void MqttClient::timerCallback(TimerHandle_t xTimer) {
    MqttClient* client = static_cast<MqttClient*>(pvTimerGetTimerID(xTimer));
    xSemaphoreGive(client->mqtt_sem_);
}

bool MqttClient::parseRealtimeAgent(const char* in_str, int in_len, room_params_t* params) {
    ESP_LOGI(TAG, "parseRealtimeAgent - input length: %d", in_len);
    
    // 检查输入字符串的有效性
    if (!in_str || in_len <= 0 || !params) {
        ESP_LOGE(TAG, "Invalid input parameters");
        return false;
    }
    
    // 限制输入长度以避免栈溢出
    if (in_len > 4096) {
        ESP_LOGE(TAG, "Input too long: %d bytes", in_len);
        return false;
    }
    
    cJSON* root = cJSON_Parse(in_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            ESP_LOGE(TAG, "JSON parse error at: %s", error_ptr);
        }
        return false;
    }

    bool success = false;
    cJSON* method = cJSON_GetObjectItem(root, "method");
    
    if (method && method->valuestring) {
        ESP_LOGI(TAG, "Found method: %s", method->valuestring);
        if (strcmp(method->valuestring, "websocket.auth.response") == 0) {
            ESP_LOGI(TAG, "Found websocket.auth.response method");
            cJSON* body = cJSON_GetObjectItem(root, "body");
            if (body) {
                ESP_LOGI(TAG, "Found body object");
                cJSON* coze_websocket = cJSON_GetObjectItem(body, "coze_websocket");
                if (coze_websocket) {
                    ESP_LOGI(TAG, "Found coze_websocket object");
                    cJSON* bot_id = cJSON_GetObjectItem(coze_websocket, "bot_id");
                    cJSON* voice_id = cJSON_GetObjectItem(coze_websocket, "voice_id");
                    cJSON* user_id = cJSON_GetObjectItem(coze_websocket, "user_id");
                    cJSON* conv_id = cJSON_GetObjectItem(coze_websocket, "conv_id");
                    cJSON* access_token = cJSON_GetObjectItem(coze_websocket, "access_token");
                    cJSON* voice_lang = cJSON_GetObjectItem(coze_websocket, "voice_lang");
                    cJSON* api_domain = cJSON_GetObjectItem(coze_websocket, "api_domain");
                    cJSON* config = cJSON_GetObjectItem(coze_websocket, "config");

                    ESP_LOGI(TAG, "Field check - bot_id: %s, voice_id: %s, user_id: %s, conv_id: %s, access_token: %s, config: %s",
                             bot_id ? "found" : "missing",
                             voice_id ? "found" : "missing",
                             user_id ? "found" : "missing",
                             conv_id ? "found" : "missing",
                             access_token ? "found" : "missing",
                             config ? "found" : "missing");

                    if (bot_id && voice_id && user_id && conv_id && access_token) {
                        ESP_LOGI(TAG, "All required fields found, copying data");
                        // 优化：使用strlcpy避免缓冲区溢出，减少内存操作
                        strlcpy(params->bot_id, bot_id->valuestring, sizeof(params->bot_id));
                        strlcpy(params->voice_id, voice_id->valuestring, sizeof(params->voice_id));
                        strlcpy(params->user_id, user_id->valuestring, sizeof(params->user_id));
                        strlcpy(params->conv_id, conv_id->valuestring, sizeof(params->conv_id));
                        strlcpy(params->access_token, access_token->valuestring, sizeof(params->access_token));
                        if (voice_lang) {
                            strlcpy(params->voice_lang, voice_lang->valuestring, sizeof(params->voice_lang));
                        }
                        if (api_domain) {
                            strlcpy(params->api_domain, api_domain->valuestring, sizeof(params->api_domain));
                        }
                        // 解析 config 字段
                        if (config && cJSON_IsObject(config)) {
                            // 优化：避免使用 cJSON_Print() 来减少内存分配
                            // 直接检查 config 对象的大小，如果太大就跳过
                            int config_item_count = cJSON_GetArraySize(config);
                            if (config_item_count > 50) {  // 限制配置项数量
                                ESP_LOGW(TAG, "Config object too large (%d items), skipping", config_item_count);
                                params->config[0] = '\0';
                            } else {
                                // 使用更安全的方法：只复制关键配置项
                                char* config_str = cJSON_PrintUnformatted(config);
                                if (config_str) {
                                    size_t config_len = strlen(config_str);
                                    if (config_len >= sizeof(params->config)) {
                                        ESP_LOGW(TAG, "Config too large (%zu bytes), truncating to %zu bytes", 
                                                 config_len, sizeof(params->config) - 1);
                                        // 截断到安全长度，确保字符串以 null 结尾
                                        strlcpy(params->config, config_str, sizeof(params->config));
                                        // 检查截断后的 JSON 是否完整
                                        if (params->config[sizeof(params->config) - 2] != '}') {
                                            ESP_LOGW(TAG, "Config JSON was truncated, may be incomplete");
                                        }
                                    } else {
                                        strlcpy(params->config, config_str, sizeof(params->config));
                                        ESP_LOGI(TAG, "Config parsed successfully (%zu bytes)", config_len);
                                    }
                                    free(config_str);  // cJSON_PrintUnformatted 分配的内存需要释放
                                    ESP_LOGI(TAG, "Config content: %s", params->config);
                                } else {
                                    ESP_LOGE(TAG, "Failed to print config JSON");
                                    params->config[0] = '\0';
                                }
                            }
                        } else {
                            // 如果没有 config 字段，设置为空字符串
                            params->config[0] = '\0';
                            ESP_LOGI(TAG, "No config field found in coze_websocket");
                        }
                        success = true;
                        ESP_LOGI(TAG, "Successfully parsed room params");
                    } else {
                        ESP_LOGE(TAG, "Missing required fields");
                    }
                } else {
                    ESP_LOGE(TAG, "coze_websocket not found");
                }
            } else {
                ESP_LOGE(TAG, "body not found");
            }
        } else {
            ESP_LOGE(TAG, "Method not websocket.auth.response: %s", method->valuestring);
        }
    } else {
        ESP_LOGE(TAG, "Method not found or null");
    }

    cJSON_Delete(root);
    return success;
}

bool MqttClient::parseM2MCtrlMsg(const char* in_str, int in_len) {
    cJSON* json = cJSON_Parse(in_str);
    if (!json) return false;

    cJSON* method = cJSON_GetObjectItem(json, "method");
    ESP_LOGI(TAG, "parseM2MCtrlMsg method: %s", method->valuestring);
    if (method && method->valuestring) {
        if (strcmp(method->valuestring, "rtc.room.join") == 0) {
            // Handle room join
            xSemaphoreGive(mqtt_sem_);

        } else if (strcmp(method->valuestring, "rtc.room.leave") == 0) {
            // Handle room leave
        } else if (strcmp(method->valuestring, "websocket.config.change") == 0) {
            getRoomInfo();
        }
    }

    cJSON_Delete(json);
    return true;
}

void MqttClient::handleMqttMessage(mqtt_msg_t* msg) {
    if (!msg) return;
    ESP_LOGI(TAG, "handleMqttMessage topic: %s, payload length: %d", msg->topic, msg->payload_len);
    
    if (strstr(msg->topic, "response")) {
        room_params_t params = {0};
        if (parseRealtimeAgent(msg->payload, msg->payload_len, &params)) {
            // 收到响应后停止定时器
            if (timer_) {
                xTimerDelete(timer_, 0);
                timer_ = nullptr;
            }
            
            // 在堆上分配RoomParams以避免栈溢出
            RoomParams* room_params = new RoomParams();
            room_params->bot_id = std::string(params.bot_id);
            room_params->voice_id = std::string(params.voice_id);
            room_params->conv_id = std::string(params.conv_id);
            room_params->access_token = std::string(params.access_token);
            room_params->voice_lang = std::string(params.voice_lang);
            room_params->api_domain = std::string(params.api_domain);
            room_params->user_id = std::string(params.user_id);
            room_params->config = std::string(params.config);
            
            ESP_LOGI(TAG, "Calling room_params_updated_callback_");
            if (room_params_updated_callback_) {
                room_params_updated_callback_(*room_params);
            } else {
                ESP_LOGE(TAG, "room_params_updated_callback_ is null");
            }
            
            delete room_params;
        }
    } else if (strstr(msg->topic, "push")) {
        parseM2MCtrlMsg(msg->payload, msg->payload_len);
    } else if (strstr(msg->topic, "ser2cli_res")) {
        auto result = iot::protocol::parse_protocol_data(
            reinterpret_cast<const uint8_t*>(msg->data), 
            msg->data_len
        );
        if (result.success && result.cmd == iot::protocol::CMD_VERSION_REPORT_RESP) {
            // Handle version report response
            if (!result.version_info.download_url.empty()) {
                // run_start_ota_task(result.version_info.module_sw_ver, result.version_info.download_url);
            }
        }
    } else if (strstr(msg->topic, "app2dev")) {
        app2devMsgHandler(reinterpret_cast<const uint8_t*>(msg->data), msg->data_len);
    }
}



uint8_t MqttClient::mqttNumRemLenBytes(const uint8_t *buf) {
    uint8_t num_bytes = 0;
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint8_t encoded_byte;

    do {
        if (num_bytes >= 4) {
            // 超过最大字节数，返回错误
            return 0;
        }
        encoded_byte = buf[num_bytes++];
        value += (encoded_byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((encoded_byte & 0x80) != 0);

    // printf("%s Buffer contents[%d]: ",__func__, num_bytes);
    // for (uint8_t i = 0; i < num_bytes; i++) {
    //     printf("%02x ", buf[i]);
    // }
    // printf("\n");
    return num_bytes;
}
// 参考数据解析函数
void MqttClient::app2devMsgHandler(const uint8_t *data, int32_t len)
{
    hexdump("app2devMsgHandler",data, len);
    // 打印前4字节
    if (len >= 4) {
        ESP_LOGI(TAG, "payload[0-3]: %02X %02X %02X %02X", data[0], data[1], data[2], data[3]);
    }
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
    uint8_t var_len = mqttNumRemLenBytes(data + 4);
    if (var_len == 0 || var_len > len - 4) {
        ESP_LOGE(TAG, "Invalid variable length");
        return;
    }
    else {
        ESP_LOGI(TAG, "var_len: %d", var_len);
    }

    // 解析Flag
    uint8_t flag = data[4 + var_len];

    // 解析命令字
    uint16_t command = (data[5 + var_len] << 8) | data[6 + var_len];
    if (command != HI_CMD_PAYLOAD93) {
        ESP_LOGE(TAG, "Invalid command");
        // todo 目前只处理93数据点业务
        return;
    }

    // 解析包序号
    uint32_t sn = (data[7 + var_len] << 24) | (data[8 + var_len] << 16) | (data[9 + var_len] << 8) | data[10 + var_len];

    // 解析业务指令
    const uint8_t *business_instruction = data + 11 + var_len;
    int business_instruction_len = len - (11 + var_len);
    if (business_instruction_len > 65535) {
        ESP_LOGE(TAG, "Business instruction too long");
        return;
    }

    // 只处理93业务指令
    ESP_LOGI(TAG, "business_instruction: %d", command);

    // 00 00 00 03 0A 00 00 93 00 00 00 00 11 10 01 
    if (command == 0x0093) {
        uint8_t action = business_instruction[0];
        ESP_LOGI(TAG, "action: 0x%02X", action);

        if (action == 0x11) {
            std::call_once(g_attrs_once, InitAttrsFromJson);
            // 拼接属性区所有字节为一个二进制串
            uint16_t bits = 0;
            for (int i = 0; i < attr_size_; ++i) {
                bits = (bits << 8) | business_instruction[1 + i];
            }
            
            // 优化：预先计算bit字段的总长度，避免重复计算
            static int total_bit_len = -1;
            static int bit_bytes = -1;
            static bool bit_bytes_calculated = false;
            
            if (!bit_bytes_calculated) {
                total_bit_len = 0;
                for (const auto& a : g_attrs) {
                    if (a.unit == "bit") {
                        total_bit_len += a.len;
                    }
                }
                bit_bytes = (total_bit_len + 7) / 8;
                bit_bytes_calculated = true;
            }
            
            int payload_bit_index = 0;
            int payload_byte_index = 0;
            
            for (int bit_index = 0; bit_index < attr_size_ * 8; ++bit_index) {
                int bit_val = (bits >> bit_index) & 0x01;
                ESP_LOGI(TAG, "bit_val: %d, bit_index: %d, attr_size: %d", bit_val, bit_index, (int)g_attrs.size());
                if (bit_index < (int)g_attrs.size()) {
                    const Attr& attr = g_attrs[bit_index];
                    if (bit_val == 1) {
                        // 数据有效，提取 attr.len 长度的数据
                        if (attr.unit == "bit") {
                            int value = 0;
                            int len = attr.len;
                            ESP_LOGI(TAG, "len: %d", len);
                            for (int l = 0; l < len; ++l) {
                                int byte_pos = 1 + attr_size_ + (payload_bit_index + l) / 8;
                                int bit_pos = (payload_bit_index + l) % 8;
                                ESP_LOGI(TAG, "byte_pos: %d", business_instruction[byte_pos]);
                                int bit = (business_instruction[byte_pos] >> bit_pos) & 0x01;
                                value |= (bit << l);
                            }
                            payload_bit_index += len;
                            ESP_LOGI(TAG, "bit attr: %s = %d", attr.name.c_str(), value);
                            processAttrValue(attr.name, value);
                        } else if (attr.unit == "byte") {
                            int len = attr.len; 
                            int byte_start = attr_size_ + bit_bytes + payload_byte_index;
                            int value = 0;
                            for (int l = 0; l < len; ++l) {
                                value |= (business_instruction[byte_start + l] << (8 * l));
                            }
                            payload_byte_index += len;
                            ESP_LOGI(TAG, "byte attr: %s = %d", attr.name.c_str(), value);
                            processAttrValue(attr.name, value);
                        }
                    }
                }
            }
        }
        
    }
}

void MqttClient::processAttrValue(std::string attr_name, int value) {
    ESP_LOGI(TAG, "processAttrValue: %s = %d", attr_name.c_str(), value);
    if (attr_name == "chat_mode") {
        Application::GetInstance().SetChatMode(value);
    }
}

// Upload binary p0 data to dev2app/<client_id_>
bool MqttClient::uploadP0Data(const void* data, size_t data_len) {
    if (!mqtt_) {
        ESP_LOGE(TAG, "MQTT client not initialized for uploadP0Data");
        return false;
    }
    std::string topic = "dev2app/" + client_id_;
    // Publish binary data (assume mqtt_->Publish can take std::string with binary data)
    // If not, this should be adapted to the actual API
    bool result = mqtt_->Publish(topic, std::string(static_cast<const char*>(data), data_len));
    if (!result) {
        ESP_LOGE(TAG, "Failed to publish p0 data to %s", topic.c_str());
    }
    return result;
}




void MqttClient::ReportTimer() {
    uint8_t binary_data[18] = {
        0x00, 0x00, 0x00, 0x03,  // 固定头部
        0x0b, 0x00, 0x00, 0x93,  // 命令标识
        0x00, 0x00, 0x00, 0x02,  // 数据长度
        0x14, 0xff,              // 数据类型
        0x00, // 0b01011011 switch，类型为bool，值为true：字段bit0，字段值为0b1；wakeup_word，类型为bool，值为true：字段bit1，字段值为0b1；charge_status，类型为enum，值为2：字段bit3 ~ bit2，字段值为0b10；alert_tone_language，类型为enum，值为1：字段bit4 ~ bit4，字段值为0b1；chat_mode，类型为enum，值为2：字段bit6 ~ bit5，字段值为0b10；          
        0x64, // 电量
        0x0a, // 音量
        0x00, // rssi
    };

    int chat_mode = Application::GetInstance().GetChatMode();
    // 使用实际的chat_mode值
    uint8_t status = 0;
    status |= (1 << 0); // switch
    status |= (1 << 1); // wakeup_word
    status |= (1 << 4); // alert_tone_language
    status |= (chat_mode << 5); // chat_mode


    auto& board = Board::GetInstance();
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        ESP_LOGI(TAG, "Battery level: %d, charging: %d, discharging: %d", level, charging, discharging);
        binary_data[15] = level;
        status |= (charging ? 1 : 0) << 2; // charge_status
        // charging = true 的时候 charge_status = 1
    }
    binary_data[14] = status;
    ESP_LOGI(TAG, "Status: %d", status);
    ESP_LOGI(TAG, "Chat mode: %d", chat_mode);

    auto codec = Board::GetInstance().GetAudioCodec();
    int volume = codec->output_volume();
    ESP_LOGI(TAG, "Volume: %d", volume);
    binary_data[16] = volume;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", ap_info.rssi);
        binary_data[17] = 100 - (uint8_t)abs(ap_info.rssi);
    }

    if (mqtt_) {
        uploadP0Data(binary_data, sizeof(binary_data));
    }

}


// const int MqttClient::attr_size_ = (8 + 8 - 1) / 8;

const char* MqttClient::kGizwitsProtocolJson = R"json(
{
    "name": "GF381",
    "packetVersion": "0x00000004",
    "protocolType": "standard",
    "product_key": "e1e1c010f6154280b5c01c69e224bdda",
    "entities": [
        {
            "display_name": "机智云开发套件",
            "attrs": [
                {
                    "display_name": "开关",
                    "name": "switch",
                    "data_type": "bool",
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "type": "status_writable",
                    "id": 0,
                    "desc": "1"
                },
                {
                    "display_name": "唤醒词",
                    "name": "wakeup_word",
                    "data_type": "bool",
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 1
                    },
                    "type": "status_writable",
                    "id": 1,
                    "desc": ""
                },
                {
                    "display_name": "充电状态",
                    "name": "charge_status",
                    "data_type": "enum",
                    "enum": [
                        "none",
                        " charging",
                        "charge_done"
                    ],
                    "position": {
                        "byte_offset": 2,
                        "unit": "bit",
                        "len": 2,
                        "bit_offset": 0
                    },
                    "type": "status_readonly",
                    "id": 5,
                    "desc": ""
                },
                {
                    "display_name": "提示音语言",
                    "name": "alert_tone_language",
                    "data_type": "enum",
                    "enum": [
                        "chinese_simplified",
                        "english"
                    ],
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 1,
                        "bit_offset": 2
                    },
                    "type": "status_writable",
                    "id": 2,
                    "desc": ""
                },
                {
                    "display_name": "chat_mode",
                    "name": "chat_mode",
                    "data_type": "enum",
                    "enum": [
                        "0",
                        "1",
                        "2"
                    ],
                    "position": {
                        "byte_offset": 0,
                        "unit": "bit",
                        "len": 2,
                        "bit_offset": 3
                    },
                    "type": "status_writable",
                    "id": 3,
                    "desc": "0 按钮\n1 唤醒词\n2 自然对话"
                },
                
                {
                    "display_name": "电量",
                    "name": "battery_percentage",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 3,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_readonly",
                    "id": 6,
                    "desc": ""
                },
                {
                    "display_name": "音量",
                    "name": "volume_set",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 1,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": 0,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_writable",
                    "id": 4,
                    "desc": ""
                },
                {
                    "display_name": "rssi",
                    "name": "rssi",
                    "data_type": "uint8",
                    "position": {
                        "byte_offset": 4,
                        "unit": "byte",
                        "len": 1,
                        "bit_offset": 0
                    },
                    "uint_spec": {
                        "addition": -100,
                        "max": 100,
                        "ratio": 1,
                        "min": 0
                    },
                    "type": "status_readonly",
                    "id": 7,
                    "desc": ""
                }
            ],
            "name": "entity0",
            "id": 0
        }
    ]
}
)json";

void MqttClient::printMemoryUsage() {
    size_t total_memory = getEstimatedMemoryUsage();
    ESP_LOGI(TAG, "MQTT Memory Usage:");
    ESP_LOGI(TAG, "  Task stacks: %zu bytes", MQTT_TASK_STACK_SIZE_RCV + MQTT_TASK_STACK_SIZE_RESEND);
    ESP_LOGI(TAG, "  Message queue: %zu bytes", MQTT_QUEUE_SIZE * sizeof(mqtt_msg_t));
    ESP_LOGI(TAG, "  Static data: %zu bytes", sizeof(g_attrs) + strlen(kGizwitsProtocolJson));
    ESP_LOGI(TAG, "  Total estimated: %zu bytes", total_memory);
    ESP_LOGI(TAG, "  Note: Increased stack size to prevent JSON parsing stack overflow");
}

size_t MqttClient::getEstimatedMemoryUsage() {
    size_t memory = 0;
    
    // 任务栈内存
    memory += MQTT_TASK_STACK_SIZE_RCV + MQTT_TASK_STACK_SIZE_RESEND;
    
    // 消息队列内存
    memory += MQTT_QUEUE_SIZE * sizeof(mqtt_msg_t);
    
    // 静态数据内存
    memory += sizeof(g_attrs);
    memory += strlen(kGizwitsProtocolJson);
    
    // 其他成员变量
    memory += sizeof(MqttClient);
    
    return memory;
}
