#pragma once

#include "soft_uart_wrapper.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <vector>
#include <cstring>

#define TAG_SOFT_UART_TASK "SoftUartTask"

// 发送数据包结构
struct SoftUartTxPacket {
    std::vector<uint8_t> data;
    SemaphoreHandle_t completion_sem;  // 发送完成信号量（可选）
};

// 接收数据包结构
struct SoftUartRxPacket {
    std::vector<uint8_t> data;
    SemaphoreHandle_t completion_sem;  // 接收完成信号量
    bool received;  // 是否已接收
};

// 软串口任务管理类
class SoftUartTask {
public:
    SoftUartTask(SoftUartWrapper* uart, gpio_num_t rx_pin)
        : uart_(uart), rx_pin_(rx_pin), task_handle_(nullptr), 
          tx_queue_(nullptr), running_(false) {}
    
    ~SoftUartTask() {
        Stop();
    }
    
    // 启动任务
    esp_err_t Start() {
        if (task_handle_ != nullptr) {
            ESP_LOGW(TAG_SOFT_UART_TASK, "Task already started");
            return ESP_ERR_INVALID_STATE;
        }
        
        // 创建发送队列（最多10个数据包）
        tx_queue_ = xQueueCreate(10, sizeof(SoftUartTxPacket*));
        if (tx_queue_ == nullptr) {
            ESP_LOGE(TAG_SOFT_UART_TASK, "Failed to create TX queue");
            return ESP_ERR_NO_MEM;
        }
        
        running_ = true;
        
        // 创建任务
        BaseType_t ret = xTaskCreate(
            TaskFunction,
            "SoftUartTask",
            4096,  // 堆栈大小
            this,
            10,    // 优先级（较高，保证实时性）
            &task_handle_
        );
        
        if (ret != pdPASS) {
            ESP_LOGE(TAG_SOFT_UART_TASK, "Failed to create task");
            vQueueDelete(tx_queue_);
            tx_queue_ = nullptr;
            running_ = false;
            return ESP_ERR_NO_MEM;
        }
        
        ESP_LOGI(TAG_SOFT_UART_TASK, "Soft UART task started");
        return ESP_OK;
    }
    
    // 停止任务
    void Stop() {
        if (task_handle_ == nullptr) {
            return;
        }
        
        running_ = false;
        
        // 等待任务结束
        if (task_handle_ != nullptr) {
            vTaskDelete(task_handle_);
            task_handle_ = nullptr;
        }
        
        // 删除队列
        if (tx_queue_ != nullptr) {
            vQueueDelete(tx_queue_);
            tx_queue_ = nullptr;
        }
        
        ESP_LOGI(TAG_SOFT_UART_TASK, "Soft UART task stopped");
    }
    
    // 发送数据（非阻塞，通过队列）
    esp_err_t Send(const uint8_t* data, size_t len, uint32_t timeout_ms = 100) {
        if (tx_queue_ == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        
        // 创建发送数据包
        SoftUartTxPacket* packet = new SoftUartTxPacket();
        packet->data.assign(data, data + len);
        packet->completion_sem = nullptr;  // 不需要等待完成
        
        // 发送到队列
        BaseType_t ret = xQueueSend(tx_queue_, &packet, pdMS_TO_TICKS(timeout_ms));
        if (ret != pdTRUE) {
            delete packet;
            ESP_LOGE(TAG_SOFT_UART_TASK, "Failed to send packet to queue");
            return ESP_ERR_TIMEOUT;
        }
        
        return ESP_OK;
    }
    
    // 接收数据（非阻塞，通过轮询检测起始位）
    esp_err_t Receive(uint8_t* data, size_t max_len, size_t& recv_len, uint32_t timeout_ms) {
        if (uart_ == nullptr || !uart_->IsInitialized()) {
            return ESP_ERR_INVALID_STATE;
        }
        
        recv_len = 0;
        
        // 轮询检测起始位（低电平）
        int64_t start_time = esp_timer_get_time() / 1000;
        bool start_bit_detected = false;
        
        while ((esp_timer_get_time() / 1000 - start_time) < timeout_ms) {
            int level = gpio_get_level(rx_pin_);
            if (level == 0) {  // 检测到起始位
                start_bit_detected = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        if (!start_bit_detected) {
            return ESP_ERR_TIMEOUT;
        }
        
        // 检测到起始位后，调用阻塞接收（此时数据已开始传输）
        std::vector<uint8_t> recv_buffer(max_len, 0);
        esp_err_t ret = uart_->Receive(recv_buffer.data(), max_len);
        if (ret != ESP_OK) {
            return ret;
        }
        
        recv_len = max_len;
        memcpy(data, recv_buffer.data(), recv_len);
        
        return ESP_OK;
    }
    
    bool IsRunning() const { return running_; }
    
private:
    // 任务函数（静态，用于xTaskCreate）
    static void TaskFunction(void* param) {
        SoftUartTask* task = static_cast<SoftUartTask*>(param);
        task->TaskLoop();
    }
    
    // 任务主循环
    void TaskLoop() {
        SoftUartTxPacket* tx_packet = nullptr;
        
        ESP_LOGI(TAG_SOFT_UART_TASK, "Task loop started");
        
        while (running_) {
            // 处理发送队列
            if (xQueueReceive(tx_queue_, &tx_packet, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (tx_packet != nullptr && uart_ != nullptr && uart_->IsInitialized()) {
                    // 发送数据（软串口发送是原子的，使用临界区保护）
                    esp_err_t ret = uart_->Send(tx_packet->data.data(), tx_packet->data.size());
                    if (ret != ESP_OK) {
                        ESP_LOGE(TAG_SOFT_UART_TASK, "Send failed: %s", esp_err_to_name(ret));
                    } else {
                        ESP_LOGD(TAG_SOFT_UART_TASK, "Sent %zu bytes", tx_packet->data.size());
                    }
                    
                    // 发送后短暂延迟，确保数据完全发送完成
                    // 9600波特率下，9字节数据大约需要 9*10*104us ≈ 9.4ms
                    // 这里延迟10ms确保数据完全发送
                    vTaskDelay(pdMS_TO_TICKS(10));
                    
                    // 通知完成（如果有信号量）
                    if (tx_packet->completion_sem != nullptr) {
                        xSemaphoreGive(tx_packet->completion_sem);
                    }
                    
                    delete tx_packet;
                    tx_packet = nullptr;
                }
            }
            
            // 可以在这里添加持续监听RX的逻辑
            // 目前接收由外部调用Receive方法触发
        }
        
        ESP_LOGI(TAG_SOFT_UART_TASK, "Task loop ended");
        vTaskDelete(NULL);
    }
    
    SoftUartWrapper* uart_;
    gpio_num_t rx_pin_;
    TaskHandle_t task_handle_;
    QueueHandle_t tx_queue_;
    bool running_;
};

