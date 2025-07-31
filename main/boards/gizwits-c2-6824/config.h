#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BOOT_BUTTON_GPIO        GPIO_NUM_7
#define BUILTIN_REC_BUTTON_GPIO   GPIO_NUM_1
// #define BOOT_BUTTON_GPIO        GPIO_NUM_9


#define CODEC_TX_GPIO           GPIO_NUM_10
#define CODEC_RX_GPIO           GPIO_NUM_18
#define BUILTIN_LED_GPIO        GPIO_NUM_6
#define BUILTIN_LED_NUM         1


#endif // _BOARD_CONFIG_H_
