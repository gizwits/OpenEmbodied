#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Movecall Moji configuration

#include <driver/gpio.h>
#include <lvgl.h>

#define CODEC_TX_GPIO           GPIO_NUM_3
#define CODEC_RX_GPIO           GPIO_NUM_9

#define POWER_GPIO              GPIO_NUM_15
#define BUILTIN_LED_GPIO        GPIO_NUM_10
#define BOOT_BUTTON_GPIO        GPIO_NUM_46
#define SLEEP_GOIO              GPIO_NUM_14

// 使用单独串口 UART_NUM_2
#define FACTORY_TEST_UART_NUM       UART_NUM_2
#define FACTORY_TEST_UART_TX_PIN    GPIO_NUM_0
#define FACTORY_TEST_UART_RX_PIN    GPIO_NUM_44

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_45
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_SPI_SCLK_PIN    GPIO_NUM_2
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_1
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_42
#define DISPLAY_SPI_DC_PIN      GPIO_NUM_47
// #define DISPLAY_SPI_RESET_PIN   GPIO_NUM_NC
#define DISPLAY_SPI_RESET_PIN   GPIO_NUM_21

#define DISPLAY_SPI_SCLK_HZ     (20 * 1000 * 1000)

#define BAT_ADC_CHANNEL  ADC_CHANNEL_2  // Battery voltage ADC channel
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11 // ADC attenuation
#define BAT_ADC_UNIT     ADC_UNIT_2
#define POWER_CHARGE_LED_PIN GPIO_NUM_NC


#endif // _BOARD_CONFIG_H_
