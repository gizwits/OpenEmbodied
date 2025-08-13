#include "test.h"
#include <iostream>
#include <unistd.h>
#include "settings.h"
#include <cstring>
#include <chrono>
#include "esp_wifi.h"
#include "audio_codec.h"
#include "esp_event.h"
#include "esp_log.h"
#include "application.h"

#include "assets/lang_config.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "cJSON.h"
#include <wifi_station.h>

#define TAG "UdpBroadcaster"

UdpBroadcaster::UdpBroadcaster(const std::string& broadcast_addr, int broadcast_port,
                             int listen_port)
    : broadcast_addr_(broadcast_addr),
      broadcast_port_(broadcast_port),
      listen_port_(listen_port),
      running_(false) {
    
    ESP_LOGI(TAG, "UdpBroadcaster constructor start");

    // // 初始化 TCP/IP 适配器
    // ESP_ERROR_CHECK(esp_netif_init());
    // esp_netif_create_default_wifi_sta();

    // // 初始化 WiFi
    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_LOGI(TAG, "Initializing WiFi...");
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // // 配置并连接 WiFi
    // wifi_config_t wifi_config = {};
    // strncpy((char*)wifi_config.sta.ssid, "Coze_2.4G", sizeof(wifi_config.sta.ssid));
    // strncpy((char*)wifi_config.sta.password, "12344321", sizeof(wifi_config.sta.password));
    
    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // ESP_ERROR_CHECK(esp_wifi_start());
    // ESP_ERROR_CHECK(esp_wifi_connect());
    // ESP_LOGI(TAG, "Connecting to WiFi...");

    // // 等待 WiFi 连接
    // int retry_count = 0;
    // wifi_ap_record_t ap_info;
    // while (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
    //     vTaskDelay(pdMS_TO_TICKS(100));
    //     if (++retry_count > 50) {  // 5秒超时
    //         ESP_LOGE(TAG, "Failed to connect to WiFi");
    //         throw std::runtime_error("Failed to connect to WiFi");
    //     }
    // }
    // ESP_LOGI(TAG, "WiFi connected");

    // // 等待 TCP/IP 栈就绪
    // ESP_LOGI(TAG, "Waiting for TCP/IP stack to be ready...");
    // vTaskDelay(pdMS_TO_TICKS(1000));  // 给 TCP/IP 栈一些时间初始化
    
    // // Create broadcast socket
    // broadcast_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    // if (broadcast_socket_ < 0) {
    //     ESP_LOGE(TAG, "Failed to create broadcast socket");
    //     throw std::runtime_error("Failed to create broadcast socket");
    // }
    // ESP_LOGI(TAG, "Broadcast socket created");

    // // Enable broadcast
    // int broadcast = 1;
    // if (setsockopt(broadcast_socket_, SOL_SOCKET, SO_BROADCAST,
    //                &broadcast, sizeof(broadcast)) < 0) {
    //     ESP_LOGE(TAG, "Failed to set broadcast option");
    //     throw std::runtime_error("Failed to set broadcast option");
    // }
    // ESP_LOGI(TAG, "Broadcast option set");

    // // Create listen socket
    // listen_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    // if (listen_socket_ < 0) {
    //     ESP_LOGE(TAG, "Failed to create listen socket");
    //     throw std::runtime_error("Failed to create listen socket");
    // }
    // ESP_LOGI(TAG, "Listen socket created");

    // // Set up listen address
    // struct sockaddr_in listen_addr;
    // memset(&listen_addr, 0, sizeof(listen_addr));
    // listen_addr.sin_family = AF_INET;
    // listen_addr.sin_addr.s_addr = INADDR_ANY;
    // listen_addr.sin_port = htons(listen_port_);

    // // Bind listen socket
    // if (bind(listen_socket_, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
    //     ESP_LOGE(TAG, "Failed to bind listen socket");
    //     throw std::runtime_error("Failed to bind listen socket");
    // }
    // ESP_LOGI(TAG, "Listen socket bound");

    ESP_LOGI(TAG, "UdpBroadcaster constructor completed");
}

UdpBroadcaster::~UdpBroadcaster() {
    stop();
    close(broadcast_socket_);
    close(listen_socket_);
}

std::string UdpBroadcaster::GetMacAddress() {
    return SystemInfo::GetMacAddress();
}

std::string UdpBroadcaster::GetFirmwareVersion() {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    return std::string(app_desc->version);
}

std::string UdpBroadcaster::GetProductKey() {
    return Auth::getInstance().getProductKey();
}

std::string UdpBroadcaster::GetHardwareVersion() {
    // 这里可以根据实际情况返回硬件版本号
    // 暂时返回空字符串
    return BOARD_NAME;
}

int UdpBroadcaster::GetVariableLengthSize(uint32_t length) {
    if (length < 128) {
        return 1;
    } else if (length < 16384) {
        return 2;
    } else if (length < 2097152) {
        return 3;
    } else {
        return 4;
    }
}

int UdpBroadcaster::EncodeVariableLength(uint32_t length, uint8_t* buffer) {
    int size = GetVariableLengthSize(length);
    for (int i = size - 1; i >= 0; i--) {
        buffer[i] = length & 0x7F;
        if (i != size - 1) {
            buffer[i] |= 0x80;
        }
        length >>= 7;
    }
    return size;
}

bool UdpBroadcaster::init_socket() {
    // 等待 TCP/IP 栈就绪
    ESP_LOGI(TAG, "Waiting for TCP/IP stack to be ready...");
    vTaskDelay(pdMS_TO_TICKS(1000));  // 给 TCP/IP 栈一些时间初始化

    // Create broadcast socket
    broadcast_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (broadcast_socket_ < 0) {
        ESP_LOGE(TAG, "Failed to create broadcast socket");
        throw std::runtime_error("Failed to create broadcast socket");
    }
    ESP_LOGI(TAG, "Broadcast socket created");

    // Enable broadcast
    int broadcast = 1;
    if (setsockopt(broadcast_socket_, SOL_SOCKET, SO_BROADCAST,
                   &broadcast, sizeof(broadcast)) < 0) {
        ESP_LOGE(TAG, "Failed to set broadcast option");
        throw std::runtime_error("Failed to set broadcast option");
    }
    ESP_LOGI(TAG, "Broadcast option set");

    // Create listen socket
    listen_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_socket_ < 0) {
        ESP_LOGE(TAG, "Failed to create listen socket");
        throw std::runtime_error("Failed to create listen socket");
    }
    ESP_LOGI(TAG, "Listen socket created");

    // Set up listen address
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(listen_port_);

    // Bind listen socket
    if (bind(listen_socket_, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind listen socket");
        throw std::runtime_error("Failed to bind listen socket");
    }
    ESP_LOGI(TAG, "Listen socket bound");
    return true;
}


void UdpBroadcaster::start() {
    if (running_) return;
    
    ESP_LOGI(TAG, "Starting broadcast and listen threads");
    running_ = true;
    
    // 等待WiFi连接成功
    while (!WifiStation::GetInstance().IsConnected()) {
        printf("#");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("\n");
    ESP_LOGI(TAG, "WiFi connected successfully");

    if (!init_socket()) {
        ESP_LOGE(TAG, "Failed to initialize sockets");
        throw std::runtime_error("Failed to initialize sockets");
    }

    // 创建广播线程
    broadcast_thread_ = std::thread(&UdpBroadcaster::broadcast_thread, this);
    // 创建监听线程
    listen_thread_ = std::thread(&UdpBroadcaster::listen_thread, this);

    auto& app = Application::GetInstance();
    auto codec = Board::GetInstance().GetAudioCodec();
    
    // 启用输入输出
    codec->EnableInput(true);
    codec->EnableOutput(true);
    
    // 使用 ESP-IDF 任务创建函数，指定更大的栈大小
    xTaskCreatePinnedToCore([](void* arg) {
        UdpBroadcaster* broadcaster = static_cast<UdpBroadcaster*>(arg);
        broadcaster->process_command_thread();
    }, "process_cmd", 8192, this, 5, &process_task_handle_, 0);
    
    while (running_) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 线程函数实现
void UdpBroadcaster::start_thread(void* arg) {
    UdpBroadcaster* broadcaster = static_cast<UdpBroadcaster*>(arg);
    broadcaster->start();
    vTaskDelete(nullptr);
}

void UdpBroadcaster::async_start() {
    ESP_LOGI(TAG, "Creating start thread");
    xTaskCreate(UdpBroadcaster::start_thread, "udp_start", 8192, this, 5, nullptr);
}

void UdpBroadcaster::stop() {
    if (!running_) return;
    
    running_ = false;
    cmd_cv_.notify_all();  // 通知处理线程退出
    
    if (broadcast_thread_.joinable()) {
        broadcast_thread_.join();
    }
    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }
    if (process_task_handle_ != nullptr) {
        vTaskDelete(process_task_handle_);
        process_task_handle_ = nullptr;
    }
}

void UdpBroadcaster::broadcast_thread() {
    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(broadcast_port_);
    broadcast_addr.sin_addr.s_addr = inet_addr(broadcast_addr_.c_str());

    while (running_) {
        // 创建 JSON 对象
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "cmd", "broadcast");
        
        // 创建 data 对象
        cJSON *data = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "data", data);

        // 添加设备信息到 data 对象
        std::string mac = GetMacAddress();
        cJSON_AddStringToObject(data, "mac", mac.c_str());

        std::string fw_ver = GetFirmwareVersion();
        cJSON_AddStringToObject(data, "fw_ver", fw_ver.c_str());

        std::string product_key = GetProductKey();
        cJSON_AddStringToObject(data, "product_key", product_key.c_str());

        std::string hw_ver = GetHardwareVersion();
        cJSON_AddStringToObject(data, "hw_ver", hw_ver.c_str());

        cJSON_AddStringToObject(data, "api_server", "api.gizwits.com:80");
        cJSON_AddStringToObject(data, "protocol_ver", "4.0.8");
        cJSON_AddStringToObject(data, "mcu_ver", "");

        // 将 JSON 对象转换为字符串
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            ESP_LOGE(TAG, "Broadcasting");
            
            // 发送数据
            if (sendto(broadcast_socket_, json_str, strlen(json_str), 0,
                      (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
                ESP_LOGE(TAG, "Failed to broadcast data");
            }
            
            // 释放 JSON 字符串
            free(json_str);
        }

        // 释放 JSON 对象
        cJSON_Delete(root);
        
        // Sleep for 1 second
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void UdpBroadcaster::listen_thread() {
    char buffer[1024];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (running_) {
        // Receive data
        ssize_t received = recvfrom(listen_socket_, buffer, sizeof(buffer) - 1, 0,
                                  (struct sockaddr*)&client_addr, &client_len);
        
        if (received > 0) {
            // 确保字符串以null结尾
            buffer[received] = '\0';
            
            ESP_LOGI(TAG, "Received %d bytes from %s:%d", received,
                     inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            ESP_LOGI(TAG, "Data: %s", buffer);
            
            // 解析 JSON 数据
            cJSON *root = cJSON_Parse(buffer);
            if (root) {
                // 获取 cmd 字段
                cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
                cJSON *target = cJSON_GetObjectItem(root, "target");
                cJSON *data = cJSON_GetObjectItem(root, "data");
                
                if (cmd && cmd->valuestring && target && target->valuestring) {
                    // 获取当前设备的 MAC 地址
                    std::string current_mac = GetMacAddress();
                    
                    // 检查 target 是否匹配当前设备的 MAC
                    if (strcmp(target->valuestring, current_mac.c_str()) == 0) {
                        // 更新最新命令
                        std::lock_guard<std::mutex> lock(cmd_mutex_);
                        Command cmd_data;
                        cmd_data.cmd = cmd->valuestring;
                        cmd_data.target = target->valuestring;
                        if (data && data->valuestring) {
                            cmd_data.data = data->valuestring;
                        }
                        cmd_data.client_addr = client_addr;
                        latest_cmd_ = cmd_data;
                        cmd_cv_.notify_one();
                    } else {
                        ESP_LOGI(TAG, "Command not for this device (target: %s, current: %s)", 
                                target->valuestring, current_mac.c_str());
                    }
                } else {
                    ESP_LOGW(TAG, "Invalid JSON format: missing cmd or target field");
                }
                
                // 释放 JSON 对象
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Failed to parse JSON data");
            }
        }
    }
}

void UdpBroadcaster::process_command_thread() {

    ESP_LOGI(TAG, "Process command thread started");

    while (running_) {
        std::optional<Command> cmd;
        {
            std::unique_lock<std::mutex> lock(cmd_mutex_);
            cmd_cv_.wait(lock, [this]() { 
                return !running_ || latest_cmd_.has_value(); 
            });
            
            if (!running_) break;
            
            if (latest_cmd_.has_value()) {
                cmd = latest_cmd_;
                latest_cmd_.reset();  // 清除命令
            } else {
                continue;
            }
        }

        // 处理命令
        if (cmd->cmd == "auth") {
            if (!cmd->data.empty()) {
                ESP_LOGI(TAG, "Processing auth command with data: %s", cmd->data.c_str());
                WriteAuthData(cmd->data.c_str(), cmd->client_addr);
            } else {
                ESP_LOGW(TAG, "Invalid auth data format: missing data field");
                SendAuthResponse(cmd->client_addr, false, "invalid data format");
            }
        } else if (cmd->cmd == "audio") {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Processing audio command");
            app.PlaySound(Lang::Sounds::P3_SUCCESS);
        } else if (cmd->cmd == "mic") {
            auto& app = Application::GetInstance();
            app.StartRecordTest(2);
            // 录音并同时播放，持续3秒
            ESP_LOGI(TAG, "Recording and playing for 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(4000)); // 等待3秒
            app.StartPlayTest(2);
            vTaskDelay(pdMS_TO_TICKS(3000)); // 等待3秒
            ESP_LOGI(TAG, "Recording and playback finished");
        } else if (cmd->cmd == "stop") {
            Settings settings("wifi", true);
            settings.SetInt("test_passed", 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else if (cmd->cmd == "reboot") {
            ESP_LOGI(TAG, "Processing reboot command");
            esp_restart();
        }
        // else if (cmd->cmd == "433_recv") {
        //     auto& app = Application::GetInstance();
        //     app.PlaySound(Lang::Sounds::P3_SUCCESS);
        //     SendResponse(cmd->client_addr, "433_recv", "success");
        // } else if (cmd->cmd == "433_send") {
        //     auto& app = Application::GetInstance();
        //     app.PlaySound(Lang::Sounds::P3_SUCCESS);
        // }
        else {
            ESP_LOGW(TAG, "Unknown command: %s", cmd->cmd.c_str());
        }
    }
}

void UdpBroadcaster::SendResponse(const struct sockaddr_in& client_addr, const char* cmd, const char* data) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "cmd", cmd);
    cJSON_AddStringToObject(response, "target", GetMacAddress().c_str());
    cJSON_AddStringToObject(response, "data", data);
    char *response_str = cJSON_PrintUnformatted(response);
    if (response_str) {
        sendto(broadcast_socket_, response_str, strlen(response_str), 0,
              (struct sockaddr*)&client_addr, sizeof(client_addr));
        free(response_str);
    }
    cJSON_Delete(response);
    
}

// 发送 auth 响应
void UdpBroadcaster::SendAuthResponse(const struct sockaddr_in& client_addr, bool success, const char* error_msg) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "cmd", "auth");
    cJSON_AddStringToObject(response, "target", GetMacAddress().c_str());
    cJSON_AddStringToObject(response, "data", success ? "success" : "fail");
    if (!success && error_msg) {
        cJSON_AddStringToObject(response, "error", error_msg);
    }
    
    char *response_str = cJSON_PrintUnformatted(response);
    if (response_str) {
        sendto(broadcast_socket_, response_str, strlen(response_str), 0,
              (struct sockaddr*)&client_addr, sizeof(client_addr));
        free(response_str);
    }
    cJSON_Delete(response);
}

// 处理 auth 数据写入
bool UdpBroadcaster::WriteAuthData(const char* data, const struct sockaddr_in& client_addr) {
    const esp_partition_t* auth_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS,
        "auth");
    
    if (!auth_partition) {
        ESP_LOGE(TAG, "Auth partition not found");
        SendAuthResponse(client_addr, false, "auth partition not found");
        return false;
    }

    esp_err_t err = esp_partition_erase_range(auth_partition, 0, auth_partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase auth partition: %s", esp_err_to_name(err));
        SendAuthResponse(client_addr, false, esp_err_to_name(err));
        return false;
    }

    err = esp_partition_write(auth_partition, 0, data, strlen(data) + 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write auth data: %s", esp_err_to_name(err));
        SendAuthResponse(client_addr, false, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Successfully wrote auth data");
    SendAuthResponse(client_addr, true);
    return true;
}
