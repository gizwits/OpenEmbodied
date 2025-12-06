#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BOOT_BUTTON_GPIO        GPIO_NUM_7
#define BUILTIN_POWER_BUTTON_GPIO   GPIO_NUM_8
// #define BOOT_BUTTON_GPIO        GPIO_NUM_9


#define CODEC_TX_GPIO           GPIO_NUM_10
#define CODEC_RX_GPIO           GPIO_NUM_18
#define BUILTIN_LED_GPIO        GPIO_NUM_6
#define BUILTIN_LED_NUM         1

#define POWER_HOLD_GPIO GPIO_NUM_0

// 使用日志串口 UART_NUM_0
#define FACTORY_TEST_UART_NUM UART_NUM_0


#define ML307_RX_PIN GPIO_NUM_19
#define ML307_TX_PIN GPIO_NUM_20

#endif // _BOARD_CONFIG_H_
