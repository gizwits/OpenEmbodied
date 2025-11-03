#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include <esp_log.h>
#include "driver/gpio.h"
#include "soft_uart.h"

// Motion command structure for queue
struct MotionCommand {
    uint8_t motion_code_a;  // data0: 动作 A
    uint8_t motion_code_b;  // data1: 动作 B
    uint32_t duration_ms;   // 持续时间
};

class SoftUartController {
public:
    SoftUartController()
        : uart_port_(NULL), task_(nullptr),
          motion_code_a_(0), motion_code_b_(0), motion_ms_remaining_(0),
          aux_flags_(0), rx_gpio_(0), enable_cb_(nullptr), enable_cb_ctx_(nullptr),
          motion_queue_(nullptr), queue_mutex_(nullptr) {}

    esp_err_t begin(uint32_t tx_gpio, uint32_t rx_gpio) {
        soft_uart_config_t cfg = {
            .tx_pin = tx_gpio,
            .rx_pin = rx_gpio,
            .baudrate = SOFT_UART_19200,
        };
        esp_err_t ret = soft_uart_new(&cfg, &uart_port_);
        if (ret != ESP_OK) return ret;
        rx_gpio_ = rx_gpio;
        // Bias RX low by default to avoid unintended sleep when floating
        gpio_set_direction((gpio_num_t)rx_gpio_, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)rx_gpio_, GPIO_PULLDOWN_ONLY);
        
        // Create motion queue (max 16 commands)
        motion_queue_ = xQueueCreate(10, sizeof(MotionCommand));
        if (motion_queue_ == NULL) {
            ESP_LOGE("SoftUart", "Failed to create motion queue");
            soft_uart_del(uart_port_);
            uart_port_ = NULL;
            return ESP_ERR_NO_MEM;
        }
        
        // Create mutex for queue operations
        queue_mutex_ = xSemaphoreCreateMutex();
        if (queue_mutex_ == NULL) {
            ESP_LOGE("SoftUart", "Failed to create queue mutex");
            vQueueDelete(motion_queue_);
            motion_queue_ = nullptr;
            soft_uart_del(uart_port_);
            uart_port_ = NULL;
            return ESP_ERR_NO_MEM;
        }
        
        // Start single task for TX and enable-state monitor
        xTaskCreate(task_entry, "su_uart", 2048, this, 5, &task_);
        return ESP_OK;
    }

    void end() {
        if (task_) {
            vTaskDelete(task_);
            task_ = nullptr;
        }
        if (motion_queue_) {
            xQueueReset(motion_queue_);
            vQueueDelete(motion_queue_);
            motion_queue_ = nullptr;
        }
        if (queue_mutex_) {
            vSemaphoreDelete(queue_mutex_);
            queue_mutex_ = nullptr;
        }
        if (uart_port_ != NULL) {
            soft_uart_del(uart_port_);
            uart_port_ = NULL;
        }
    }

    // Set callback for enable-state: working=true when RX is low; working=false when RX is high
    void setEnableStateCallback(void (*cb)(bool working, void* ctx), void* ctx) {
        enable_cb_ = cb;
        enable_cb_ctx_ = ctx;
    }

    void sendAction(uint8_t action_code, uint8_t data2 = 0x00) {
        sendFrame(action_code, aux_flags_, data2);
    }
    // Start dual motion: action A and action B simultaneously
    // Protocol: data0 = motion_code_a, data1 = motion_code_b, data2 = reserved (0x00)
    void startMotion(uint8_t motion_code_a, uint8_t motion_code_b, uint32_t duration_ms) {
        if (motion_queue_ == nullptr) {
            ESP_LOGW("SoftUart", "Motion queue not initialized");
            return;
        }
        
        MotionCommand cmd;
        cmd.motion_code_a = motion_code_a;
        cmd.motion_code_b = motion_code_b;
        cmd.duration_ms = duration_ms;
        
        // Add to queue (non-blocking)
        if (xQueueSendToBack(motion_queue_, &cmd, 0) != pdTRUE) {
            ESP_LOGW("SoftUart", "Motion queue full, dropping command A=0x%02X B=0x%02X", 
                     motion_code_a, motion_code_b);
        } else {
            ESP_LOGI("SoftUart", "Motion command queued: A=0x%02X B=0x%02X, duration: %lu ms", 
                     motion_code_a, motion_code_b, (unsigned long)duration_ms);
        }
    }

    // Stop motion and send 0x00, clear queue
    void stopMotion() {
        if (queue_mutex_ && xSemaphoreTake(queue_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            motion_code_a_ = 0x00;
            motion_code_b_ = 0x00;
            motion_ms_remaining_ = 0;
            if (motion_queue_) {
                xQueueReset(motion_queue_);
            }
            xSemaphoreGive(queue_mutex_);
            sendFrame(0x00, 0x00, 0x00);
        }
    }
    
    // Clear motion queue without stopping current motion
    void clearQueue() {
        if (motion_queue_) {
            xQueueReset(motion_queue_);
            ESP_LOGI("SoftUart", "Motion queue cleared");
        }
    }

    // Send explicit stop once
    void sendStop() { sendFrame(0x00, aux_flags_, 0x00); }

private:
    static constexpr uint8_t kHeader = 0xAA;

    static void task_entry(void* arg) {
        static_cast<SoftUartController*>(arg)->taskLoop();
    }
    

    void taskLoop() {
        int last_level = gpio_get_level((gpio_num_t)rx_gpio_);
        int elapsed_ms = 0;
        int ignore_changes = 1; // ignore the first detected edge after boot
        for (;;) {
            // Poll RX enable state every 50ms
            int level = gpio_get_level((gpio_num_t)rx_gpio_);
            if (level != last_level) {
                last_level = level;
                if (ignore_changes > 0) {
                    ignore_changes--;
                } else {
                    bool working = (level == 0);
                    if (enable_cb_) {
                        enable_cb_(working, enable_cb_ctx_);
                    }
                }
            }

            // Process motion queue: check if we need to start a new motion
            if (motion_code_a_ == 0 && motion_code_b_ == 0 && motion_queue_ != nullptr) {
                MotionCommand cmd;
                if (xQueueReceive(motion_queue_, &cmd, 0) == pdTRUE) {
                    // Start new motion from queue
                    if (queue_mutex_ && xSemaphoreTake(queue_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                        motion_code_a_ = cmd.motion_code_a;
                        motion_code_b_ = cmd.motion_code_b;
                        motion_ms_remaining_ = cmd.duration_ms;
                        xSemaphoreGive(queue_mutex_);
                        ESP_LOGI("SoftUart", "Starting queued motion: A=0x%02X B=0x%02X, duration: %lu ms",
                                 cmd.motion_code_a, cmd.motion_code_b, (unsigned long)cmd.duration_ms);
                    }
                }
            }

            // Accumulate for motion TX
            elapsed_ms += 50;
            if (elapsed_ms >= 100) {
                elapsed_ms = 0;
                uint8_t current_a = motion_code_a_;
                uint8_t current_b = motion_code_b_;
                if (current_a != 0 || current_b != 0) {
                    // Send frame with both actions: data0 = action A, data1 = action B
                    sendFrame(current_a, current_b, 0x00);
                    if (motion_ms_remaining_ > 0) {
                        if (queue_mutex_ && xSemaphoreTake(queue_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
                            motion_ms_remaining_ -= 100;
                            if (motion_ms_remaining_ <= 0) {
                                // Current motion finished, stop and prepare for next
                                motion_code_a_ = 0x00;
                                motion_code_b_ = 0x00;
                                motion_ms_remaining_ = 0;
                                // Send stop frame
                                sendFrame(0x00, 0x00, 0x00);
                                ESP_LOGI("SoftUart", "Motion completed, checking queue for next");
                            }
                            xSemaphoreGive(queue_mutex_);
                        }
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void sendFrame(uint8_t data0, uint8_t data1, uint8_t data2) {
        if (uart_port_ == NULL) return;
        uint8_t frame[5];
        frame[0] = kHeader;
        frame[1] = data0;
        frame[2] = data1;
        frame[3] = data2;
        uint16_t sum = (uint16_t)data0 + (uint16_t)data1 + (uint16_t)data2;
        frame[4] = (uint8_t)(sum & 0xFF);
        soft_uart_send(uart_port_, frame, sizeof(frame));
    }

    void setFlag(uint8_t bit, bool on) {
        if (on) aux_flags_ |= bit; else aux_flags_ &= ~bit;
    }

    void enableTaskLoop() {
        int last = -1;
        for (;;) {
            int level = gpio_get_level((gpio_num_t)rx_gpio_);
            if (level != last) {
                last = level;
                bool working = (level == 0);
                if (enable_cb_) {
                    enable_cb_(working, enable_cb_ctx_);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

private:
    soft_uart_port_t uart_port_;
    TaskHandle_t task_;
    volatile uint8_t motion_code_a_;  // data0: 动作 A
    volatile uint8_t motion_code_b_;  // data1: 动作 B
    volatile int32_t motion_ms_remaining_;
    volatile uint8_t aux_flags_;
    uint32_t rx_gpio_;
    void (*enable_cb_)(bool, void*);
    void* enable_cb_ctx_;
    QueueHandle_t motion_queue_;
    SemaphoreHandle_t queue_mutex_;
};


