#ifndef TEST_H
#define TEST_H

#include <string>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "system_info.h"
#include "esp_app_desc.h"
#include "auth.h"
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>

// Default ports
#define DEFAULT_BROADCAST_PORT 2415
#define DEFAULT_LISTEN_PORT 12414

// Broadcast data structure
struct BroadcastData {
    uint32_t header;        // Fixed header: 0x00000003
    uint8_t var_len;        // Length of variable data
    uint8_t flag;          // Flag: 0x00
    uint16_t cmd;          // Command: 0x0004
    uint16_t did_len;      // Length of DID
    char did[23];          // Device ID (max 22 chars + null terminator)
    uint16_t mac_len;      // Length of MAC
    char mac[32];          // MAC address
    uint16_t fw_ver_len;   // Length of firmware version
    char fw_ver[32];       // Firmware version
    uint16_t product_key_len;  // Length of product key
    char product_key[32];      // Product key
    uint64_t device_attr;      // Device attributes (8 bytes)
    char api_server[64];       // API server domain
    char protocol_ver[16];     // Protocol version
    char mcu_ver[8];          // MCU software version
    uint16_t hw_ver_len;       // Length of hardware version
    char hw_ver[32];          // Hardware version
} __attribute__((packed));

class UdpBroadcaster {
public:
    UdpBroadcaster(const std::string& broadcast_addr = "255.255.255.255",
                   int broadcast_port = DEFAULT_BROADCAST_PORT,
                   int listen_port = DEFAULT_LISTEN_PORT);
    ~UdpBroadcaster();

    // Start broadcasting and listening
    void start();

    // Start broadcasting and listening asynchronously
    void async_start();

    // Stop broadcasting and listening
    void stop();

private:
    // Broadcast thread function
    void broadcast_thread();
    // Listen thread function
    void listen_thread();
    // Command processing thread function
    void process_command_thread();

    // Get MAC address
    std::string GetMacAddress();
    // Get firmware version
    std::string GetFirmwareVersion();
    // Get product key
    std::string GetProductKey();
    // Get hardware version
    std::string GetHardwareVersion();

    // MQTT variable length encoding
    static int EncodeVariableLength(uint32_t length, uint8_t* buffer);
    static int GetVariableLengthSize(uint32_t length);

    // Auth related helper functions
    void SendAuthResponse(const struct sockaddr_in& client_addr, bool success, const char* error_msg = nullptr);
    bool WriteAuthData(const char* data, const struct sockaddr_in& client_addr);
    void SendResponse(const struct sockaddr_in& client_addr, const char* cmd, const char* data);

    // 新增声明
    bool init_socket();
    static void start_thread(void* arg);

    std::string broadcast_addr_;
    int broadcast_port_;
    int listen_port_;
    
    int broadcast_socket_;
    int listen_socket_;
    
    std::atomic<bool> running_;
    std::thread broadcast_thread_;
    std::thread listen_thread_;
    TaskHandle_t process_task_handle_{nullptr};  // 处理任务的句柄

    // Command processing related
    std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    struct Command {
        std::string cmd;
        std::string target;
        std::string data;
        struct sockaddr_in client_addr;
    };
    std::optional<Command> latest_cmd_;  // 只保存最新的命令
    std::atomic<bool> processing_cmd_{false};
};

#endif // TEST_H
