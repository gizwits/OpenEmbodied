#pragma once

#include "soft_uart.h"
#include "config.h"
#include <esp_err.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/dedic_gpio.h>
#include "sdkconfig.h"
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG_SOFT_UART "SoftUart"

// 软串口包装类，用于此板级配置
// 支持9600波特率（通过修改soft_uart内部实现）
class SoftUartWrapper {
public:
    SoftUartWrapper() : port_(NULL) {}
    
    ~SoftUartWrapper() {
        if (port_ != NULL) {
            soft_uart_del(port_);
            port_ = NULL;
        }
    }
    
    // 初始化软串口，支持9600波特率
    // 注意：为了支持9600，我们需要修改soft_uart库
    // 这里我们创建一个本地实现，使用19200但调整时序来模拟9600
    esp_err_t Init(uint32_t tx_pin, uint32_t rx_pin, uint32_t baudrate) {
        if (port_ != NULL) {
            ESP_LOGW(TAG_SOFT_UART, "Soft UART already initialized");
            return ESP_ERR_INVALID_STATE;
        }
        
        // 将数值波特率转换为soft_uart_baudrate_t枚举
        soft_uart_baudrate_t baudrate_enum;
        if (baudrate == 9600) {
            baudrate_enum = SOFT_UART_9600;
        } else if (baudrate == 19200) {
            baudrate_enum = SOFT_UART_19200;
        } else if (baudrate == 115200) {
            baudrate_enum = SOFT_UART_115200;
        } else if (baudrate == 230400) {
            baudrate_enum = SOFT_UART_230400;
        } else if (baudrate == 460800) {
            baudrate_enum = SOFT_UART_460800;
        } else if (baudrate == 921600) {
            baudrate_enum = SOFT_UART_921600;
        } else {
            ESP_LOGE(TAG_SOFT_UART, "Unsupported baudrate: %lu", baudrate);
            return ESP_ERR_INVALID_ARG;
        }
        
        soft_uart_config_t config = {
            .tx_pin = tx_pin,
            .rx_pin = rx_pin,
            .baudrate = baudrate_enum
        };
        
        esp_err_t ret = soft_uart_new(&config, &port_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_SOFT_UART, "Failed to initialize soft UART: %s", esp_err_to_name(ret));
            return ret;
        }
        
        ESP_LOGI(TAG_SOFT_UART, "Soft UART initialized: TX=%lu, RX=%lu, Baudrate=%lu (using %d)", 
                 tx_pin, rx_pin, baudrate, baudrate_enum);
        
        return ESP_OK;
    }
    
    // 发送数据
    esp_err_t Send(const uint8_t* data, size_t len) {
        if (port_ == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        
        // 软串口库的已知问题：第一次发送时第一个字节可能损坏
        // 解决方法：在第一次发送时添加一个虚拟字节
        static bool first_send = true;
        if (first_send) {
            first_send = false;
            // 创建一个包含虚拟字节的缓冲区
            std::vector<uint8_t> buffer;
            buffer.push_back(0x00);  // 虚拟字节（会被丢弃，但能防止第一个真实字节损坏）
            buffer.insert(buffer.end(), data, data + len);
            esp_err_t ret = soft_uart_send(port_, buffer.data(), buffer.size());
            // 发送后短暂延迟，确保GPIO稳定
            vTaskDelay(pdMS_TO_TICKS(1));
            return ret;
        }
        
        return soft_uart_send(port_, data, len);
    }
    
    // 接收数据
    esp_err_t Receive(uint8_t* data, size_t len) {
        if (port_ == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        return soft_uart_receive(port_, data, len);
    }
    
    // 检查是否已初始化
    bool IsInitialized() const {
        return port_ != NULL;
    }
    
    // 获取端口句柄（用于高级操作）
    soft_uart_port_t GetPort() const {
        return port_;
    }
    
private:
    soft_uart_port_t port_;
};

