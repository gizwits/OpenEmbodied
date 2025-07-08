#include "giz_mqtt.h"
#include "giz_api.h"
#include "board.h"
#include <esp_log.h>
#include <ml307_mqtt.h>
#include "protocol/iot_protocol.h"
#include <cstring>
#include "auth.h"
#include <arpa/inet.h>
#include "application.h"
#include "settings.h"
#include <esp_wifi.h>
#include "audio_codecs/audio_codec.h"

#define TAG "GIZ_MQTT"

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
        if (message_callback_) {
            message_callback_(topic, payload);
        }

        // ESP_LOGI(TAG, "OnMessage topic: %s, payload: %s", topic.c_str(), payload.c_str());
        
        mqtt_msg_t msg = {0};
        msg.topic = strdup(topic.c_str());
        msg.topic_len = topic.length();
        msg.payload = strdup(payload.c_str());
        msg.payload_len = payload.length();
        msg.data = msg.payload;
        msg.data_len = msg.payload_len;
        
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
    message_queue_ = xQueueCreate(20, sizeof(mqtt_msg_t));
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
    xTaskCreate(messageReceiveHandler, "mqtt_rcv", 4096, this, 5, nullptr);
    xTaskCreate(messageResendHandler, "mqtt_resend", 4096, this, 5, nullptr);

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
    return true;
}

bool MqttClient::publish(const std::string& topic, const std::string& payload) {
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return false;
    }
    ESP_LOGI(TAG, "publish topic: %s, payload: %s", topic.c_str(), payload.c_str());
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
    char msg[256];
    int len = snprintf(msg, sizeof(msg),
        "{\r\n"
        "    \"method\": \"token.report\",\r\n"
        "    \"body\": {\r\n"
        "        \"total\": %d,\r\n"
        "        \"output\": %d,\r\n"
        "        \"input\": %d\r\n"
        "    }\r\n"
        "}", total, output, input);

    if (len > 0 && len < sizeof(msg)) {
        publish("report", std::string(msg, len));
    }
}

void MqttClient::OnRoomParamsUpdated(std::function<void(const RoomParams&)> callback) {
    room_params_updated_callback_ = callback;
}

bool MqttClient::getRoomInfo() {
    const char* msg = 
        "{\r\n"
        "    \"method\": \"websocket.auth.request\"\r\n"
        "}";

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
            free(msg.topic);
            free(msg.payload);
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
    cJSON* root = cJSON_Parse(in_str);
    if (!root) return false;

    bool success = false;
    cJSON* method = cJSON_GetObjectItem(root, "method");
    
    if (method && strcmp(method->valuestring, "websocket.auth.response") == 0) {
        cJSON* body = cJSON_GetObjectItem(root, "body");
        if (body) {
            cJSON* coze_websocket = cJSON_GetObjectItem(body, "coze_websocket");
            if (coze_websocket) {
                cJSON* bot_id = cJSON_GetObjectItem(coze_websocket, "bot_id");
                cJSON* voice_id = cJSON_GetObjectItem(coze_websocket, "voice_id");
                cJSON* user_id = cJSON_GetObjectItem(coze_websocket, "user_id");
                cJSON* conv_id = cJSON_GetObjectItem(coze_websocket, "conv_id");
                cJSON* access_token = cJSON_GetObjectItem(coze_websocket, "access_token");
                cJSON* voice_lang = cJSON_GetObjectItem(coze_websocket, "voice_lang");
                cJSON* api_domain = cJSON_GetObjectItem(coze_websocket, "api_domain");


                if (bot_id && voice_id && user_id && conv_id && access_token) {
                    strncpy(params->bot_id, bot_id->valuestring, sizeof(params->bot_id) - 1);
                    strncpy(params->voice_id, voice_id->valuestring, sizeof(params->voice_id) - 1);
                    strncpy(params->user_id, user_id->valuestring, sizeof(params->user_id) - 1);
                    strncpy(params->conv_id, conv_id->valuestring, sizeof(params->conv_id) - 1);
                    strncpy(params->access_token, access_token->valuestring, sizeof(params->access_token) - 1);
                    strncpy(params->voice_lang, voice_lang->valuestring, sizeof(params->voice_lang) - 1);
                    strncpy(params->api_domain, api_domain->valuestring, sizeof(params->api_domain) - 1);
                    success = true;
                }
            }
        }
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
    ESP_LOGI(TAG, "handleMqttMessage topic: %s, payload: %s", msg->topic, msg->payload);
    if (strstr(msg->topic, "response")) {
        room_params_t params = {0};
        if (parseRealtimeAgent(msg->payload, msg->payload_len, &params)) {
            // 收到响应后停止定时器
            if (timer_) {
                xTimerDelete(timer_, 0);
                timer_ = nullptr;
            }
            RoomParams room_params;
            room_params.bot_id = params.bot_id;
            room_params.voice_id = params.voice_id;
            room_params.conv_id = params.conv_id;
            room_params.access_token = params.access_token;
            room_params.voice_lang = params.voice_lang;
            room_params.api_domain = params.api_domain;
            room_params.user_id = params.user_id;
            room_params_updated_callback_(room_params);
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
        0x64, // 音量
        0x0a, // 电量
        0x00, // rssi
    };

    int chat_mode = Application::GetInstance().GetChatMode();
    // chat_mode 固定填充 1
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
        binary_data[16] = level;
        status |= (charging ? 1 : 0) << 2; // charge_status
        // charging = true 的时候 charge_status = 1
    }
    binary_data[14] = status;
    ESP_LOGI(TAG, "Status: %d", status);

    auto codec = Board::GetInstance().GetAudioCodec();
    int volume = codec->output_volume();
    ESP_LOGI(TAG, "Volume: %d", volume);
    binary_data[15] = volume;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi RSSI: %d dBm", ap_info.rssi);
        binary_data[17] = 100 - (uint8_t)abs(ap_info.rssi);
    }

    if (mqtt_) {
        uploadP0Data(binary_data, sizeof(binary_data));
    }

}