#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Gizwits S3 VB6824 with ST7735S 0.96 inch TFT configuration

#include <driver/gpio.h>
#include <lvgl.h>

#define CODEC_TX_GPIO           GPIO_NUM_3
#define CODEC_RX_GPIO           GPIO_NUM_9

// Power management pins

#define POWER_HOLD_GPIO              GPIO_NUM_42
#define BOOT_BUTTON_GPIO        GPIO_NUM_2
#define POWER_BUTTON_GPIO GPIO_NUM_41


// 使用单独串口 UART_NUM_2
#define FACTORY_TEST_UART_NUM       UART_NUM_0
#define FACTORY_TEST_UART_TX_PIN    GPIO_NUM_43
#define FACTORY_TEST_UART_RX_PIN    GPIO_NUM_44

// ST7735S display configuration (根据实际屏幕尺寸)
// For landscape mode, swap the dimensions
#define DISPLAY_WIDTH   160
#define DISPLAY_HEIGHT  128
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true
#define DISPLAY_SWAP_XY true

// ST7735S display offset (for 128x160 TFT)
// Match test.py/ST7735S.py behavior first
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_45
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

// ST7735S SPI pins (根据实际硬件配置)
#define DISPLAY_SPI_SCLK_PIN    GPIO_NUM_48
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_40
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_38
#define DISPLAY_SPI_DC_PIN      GPIO_NUM_39
#define DISPLAY_SPI_RESET_PIN   GPIO_NUM_21

// Start very low like MicroPython test (then raise later)
#define DISPLAY_SPI_SCLK_HZ     (4 * 1000 * 1000)

#define BAT_ADC_CHANNEL  ADC_CHANNEL_2  // Battery voltage ADC channel
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11 // ADC attenuation
#define BAT_ADC_UNIT     ADC_UNIT_2
#define POWER_CHARGE_LED_PIN GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
