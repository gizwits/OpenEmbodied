#pragma once

// 确保包含boards/common/protocol.h，定义SerialInterface和ProtocolErrCode
#include "boards/common/protocol.h"
#include "soft_uart_task.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <vector>
#include <cstring>
#include <algorithm>

#define TAG_SOFT_UART_SERIAL "SoftUartSerial"

// 软串口的SerialInterface实现，用于Gizwits_Protocol类
// 使用专门的任务来处理软串口收发，提供更好的实时性
class SoftUartSerial : public SerialInterface {
public:
    SoftUartSerial(SoftUartTask* uart_task) 
        : uart_task_(uart_task) {}
    
    virtual ~SoftUartSerial() = default;
    
    // 发送数据（通过任务队列，非阻塞）
    virtual ProtocolErrCode send(const uint8_t* data, uint16_t len) override {
        if (uart_task_ == nullptr || !uart_task_->IsRunning()) {
            ESP_LOGE(TAG_SOFT_UART_SERIAL, "Soft UART task not running");
            return ProtocolErrCode::INVALID_PACKET;
        }
        
        esp_err_t ret = uart_task_->Send(data, len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_SOFT_UART_SERIAL, "Send failed: %s", esp_err_to_name(ret));
            return ProtocolErrCode::INVALID_PACKET;
        }
        
        return ProtocolErrCode::SUCCESS;
    }
    
    // 接收数据（通过任务，非阻塞方式）
    virtual ProtocolErrCode receive(uint8_t* data, uint16_t max_len,
                                    uint16_t timeout_ms, uint16_t& recv_len) override {
        if (uart_task_ == nullptr || !uart_task_->IsRunning()) {
            ESP_LOGE(TAG_SOFT_UART_SERIAL, "Soft UART task not running");
            return ProtocolErrCode::INVALID_PACKET;
        }
        
        recv_len = 0;
        size_t recv_len_size = 0;
        
        esp_err_t ret = uart_task_->Receive(data, max_len, recv_len_size, timeout_ms);
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG_SOFT_UART_SERIAL, "Timeout waiting for data");
            return ProtocolErrCode::TIMEOUT_ERROR;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG_SOFT_UART_SERIAL, "Receive failed: %s", esp_err_to_name(ret));
            return ProtocolErrCode::TIMEOUT_ERROR;
        }
        
        recv_len = static_cast<uint16_t>(recv_len_size);
        ESP_LOGD(TAG_SOFT_UART_SERIAL, "Received %d bytes", recv_len);
        return ProtocolErrCode::SUCCESS;
    }
    
private:
    SoftUartTask* uart_task_;  // 软串口任务实例
};

