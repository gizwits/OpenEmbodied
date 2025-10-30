#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include "driver/gpio.h"
#include "soft_uart.h"

class SoftUartController {
public:
    SoftUartController()
        : uart_port_(NULL), task_(nullptr),
          motion_code_(0), motion_ms_remaining_(0),
          aux_flags_(0), rx_gpio_(0), enable_cb_(nullptr), enable_cb_ctx_(nullptr) {}

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
        // Start single task for TX and enable-state monitor
        xTaskCreate(task_entry, "su_uart", 2048, this, 5, &task_);
        return ESP_OK;
    }

    void end() {
        if (task_) {
            vTaskDelete(task_);
            task_ = nullptr;
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

    // Set auxiliary flags in data1
    void setShoot(bool on) { setFlag(0x08, on); }
    void setMute(bool on) { setFlag(0x40, on); }
    void setLight(bool on) { setFlag(0x80, on); }

    // One-shot action (non-directional)
    void sendAction(uint8_t action_code, uint8_t data2 = 0x00) {
        sendFrame(action_code, aux_flags_, data2);
    }

    // Start continuous motion at 100ms interval
    void startMotion(uint8_t motion_code) {
        motion_code_ = motion_code; // e.g., 0x02 forward, 0x03 back...
        motion_ms_remaining_ = 0;   // 0 = no auto-stop
    }

    // Start motion and auto-stop after duration_ms
    void startMotion(uint8_t motion_code, uint32_t duration_ms) {
        motion_code_ = motion_code;
        motion_ms_remaining_ = (int32_t)duration_ms;
    }

    // Stop motion and send 0x00
    void stopMotion() {
        motion_code_ = 0x00;
        sendFrame(0x00, aux_flags_, 0x00);
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

            // Accumulate for motion TX
            elapsed_ms += 50;
            if (elapsed_ms >= 100) {
                elapsed_ms = 0;
                uint8_t current = motion_code_;
                if (current != 0) {
                    sendFrame(current, aux_flags_, 0x00);
                    if (motion_ms_remaining_ > 0) {
                        motion_ms_remaining_ -= 100;
                        if (motion_ms_remaining_ <= 0) {
                            stopMotion();
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
    volatile uint8_t motion_code_;
    volatile int32_t motion_ms_remaining_;
    volatile uint8_t aux_flags_;
    uint32_t rx_gpio_;
    void (*enable_cb_)(bool, void*);
    void* enable_cb_ctx_;
};


