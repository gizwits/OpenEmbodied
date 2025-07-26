#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>


#define CODEC_TX_GPIO           GPIO_NUM_10
#define CODEC_RX_GPIO           GPIO_NUM_18

#define BOOT_BUTTON_GPIO        GPIO_NUM_5
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_7
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_1
#define PREV_BUTTON_GPIO GPIO_NUM_2
#define NEXT_BUTTON_GPIO GPIO_NUM_4

#endif // _BOARD_CONFIG_H_
