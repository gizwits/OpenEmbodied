#pragma once

#include "soft_uart.h"

// 软串口配置
// 用户可以根据需要修改这些配置

// GPIO引脚配置
#define SOFT_UART_TX_PIN 20  // 发送引脚，可以根据需要修改
#define SOFT_UART_RX_PIN 19  // 接收引脚，可以根据需要修改

// 波特率配置
// 可选值：SOFT_UART_115200, SOFT_UART_230400, SOFT_UART_460800, SOFT_UART_921600
#define SOFT_UART_BAUDRATE SOFT_UART_115200

// 日志配置
#define SOFT_UART_LOG_ENABLED 0        // 是否启用软串口日志输出
#define SOFT_UART_LOG_BUFFER_SIZE 512  // 日志缓冲区大小
#define SOFT_UART_LOG_PREFIX_SIZE 128  // 日志前缀缓冲区大小

// 调试配置
#define SOFT_UART_DEBUG_ENABLED 0      // 是否启用调试信息

// 引脚说明：
// - TX_PIN: 连接到USB转串口模块的RX端，或连接到其他设备的RX端
// - RX_PIN: 连接到USB转串口模块的TX端，或连接到其他设备的TX端
// 
// 常见连接方式：
// 1. USB转串口模块：
//    - ESP32 TX_PIN -> USB转串口模块 RX
//    - ESP32 RX_PIN -> USB转串口模块 TX
//    - ESP32 GND -> USB转串口模块 GND
//
// 2. 其他ESP32设备：
//    - ESP32 TX_PIN -> 其他ESP32 RX_PIN
//    - ESP32 RX_PIN -> 其他ESP32 TX_PIN
//    - ESP32 GND -> 其他ESP32 GND
//