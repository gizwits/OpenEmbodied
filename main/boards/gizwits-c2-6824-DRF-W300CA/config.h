#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 按键配置
#define BOOT_BUTTON_GPIO        GPIO_NUM_8     // BOOT按键
#define KEY_ADC_CHANNEL         1              // ADC1通道1
#define KEY_GPIO                GPIO_NUM_1     // 连接到KEY的GPIO

// 音频编解码配置
#define CODEC_TX_GPIO           GPIO_NUM_10
#define CODEC_RX_GPIO           GPIO_NUM_18

// LED 配置
#define BUILTIN_LED_GPIO        GPIO_NUM_6

// 电源控制配置
#define POWER_HOLD_GPIO        GPIO_NUM_0     // 电源保持引脚

// RGB LED 配置（V 通道恒接 VCC，无需控制）
#define RGB_LED_R_GPIO          GPIO_NUM_5   // 红色（R）
#define RGB_LED_G_GPIO          GPIO_NUM_4   // 绿色（G）
#define RGB_LED_B_GPIO          GPIO_NUM_7   // 蓝色（B）

// 电机控制配置
#define MOTOR_CTRL_GPIO         GPIO_NUM_2

#endif // _BOARD_CONFIG_H_
