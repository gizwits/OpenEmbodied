#include "minimal_ws2812.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "MinimalWS2812"

// WS2812时序参数（基于80MHz CPU）
#define T0H_NS 400    // 0码高电平时间 (ns)
#define T0L_NS 850    // 0码低电平时间 (ns)
#define T1H_NS 800    // 1码高电平时间 (ns)
#define T1L_NS 450    // 1码低电平时间 (ns)
#define RESET_NS 50000 // 复位时间 (ns)

// 将纳秒转换为CPU周期数（80MHz = 12.5ns/cycle）
#define NS_TO_CYCLES(ns) ((ns) / 12.5)

MinimalWS2812::MinimalWS2812(gpio_num_t gpio, uint8_t led_count) 
    : gpio_(gpio), led_count_(led_count) {
    
    // 分配像素数据内存（每个LED 3字节：G, R, B）
    pixel_data_ = new uint8_t[led_count_ * 3];
    memset(pixel_data_, 0, led_count_ * 3);
    
    // 配置GPIO
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << gpio_),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    gpio_set_level(gpio_, 0);
    
    ESP_LOGI(TAG, "MinimalWS2812 initialized on GPIO %d, %d LEDs", gpio_, led_count_);
}

MinimalWS2812::~MinimalWS2812() {
    delete[] pixel_data_;
}

void MinimalWS2812::SendBit(bool bit) {
    if (bit) {
        // 发送1码：高电平800ns，低电平450ns
        gpio_set_level(gpio_, 1);
        for (volatile int i = 0; i < NS_TO_CYCLES(T1H_NS); i++);
        gpio_set_level(gpio_, 0);
        for (volatile int i = 0; i < NS_TO_CYCLES(T1L_NS); i++);
    } else {
        // 发送0码：高电平400ns，低电平850ns
        gpio_set_level(gpio_, 1);
        for (volatile int i = 0; i < NS_TO_CYCLES(T0H_NS); i++);
        gpio_set_level(gpio_, 0);
        for (volatile int i = 0; i < NS_TO_CYCLES(T0L_NS); i++);
    }
}

void MinimalWS2812::SendByte(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        SendBit((byte >> i) & 1);
    }
}

void MinimalWS2812::SendReset() {
    gpio_set_level(gpio_, 0);
    vTaskDelay(pdMS_TO_TICKS(1)); // 1ms复位时间
}

void MinimalWS2812::SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= led_count_) return;
    
    uint8_t* pixel = &pixel_data_[index * 3];
    pixel[0] = g;  // WS2812格式：G, R, B
    pixel[1] = r;
    pixel[2] = b;
}

void MinimalWS2812::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    // 设置所有LED为相同颜色
    for (uint8_t i = 0; i < led_count_; i++) {
        SetPixel(i, r, g, b);
    }
    
    // 发送数据
    for (uint8_t i = 0; i < led_count_; i++) {
        uint8_t* pixel = &pixel_data_[i * 3];
        SendByte(pixel[0]); // G
        SendByte(pixel[1]); // R
        SendByte(pixel[2]); // B
    }
    SendReset();
}

void MinimalWS2812::TurnOff() {
    SetColor(0, 0, 0);
}
