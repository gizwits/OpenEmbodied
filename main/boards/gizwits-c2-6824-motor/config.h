#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
// 电机驱动板GPIO映射
// io4  io2  io1  io7
// in1  in3 in4 in2

#define CODEC_TX_GPIO           GPIO_NUM_10
#define CODEC_RX_GPIO           GPIO_NUM_18

// #define LED_GPIO        GPIO_NUM_6
#define EXTRA_LIGHT_GPIO GPIO_NUM_2
// #define POWER_GPIO GPIO_NUM_9

// 使用日志串口 UART_NUM_0
#define FACTORY_TEST_UART_NUM UART_NUM_0
#define FACTORY_TEST_UART_TX_PIN    GPIO_NUM_20
#define FACTORY_TEST_UART_RX_PIN    GPIO_NUM_19

#endif // _BOARD_CONFIG_H_
