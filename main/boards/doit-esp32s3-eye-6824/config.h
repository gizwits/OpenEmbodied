/*
 * @Description: 
 * @Author: cjs丶
 * @Date: 2025-06-11 09:29:39
 * @LastEditTime: 2025-06-18 09:22:34
 * @LastEditors: cjs丶
 */
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define CODEC_TX_GPIO           GPIO_NUM_17
#define CODEC_RX_GPIO           GPIO_NUM_18

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define SLEEP_GOIO              GPIO_NUM_9

//分辨率
#if CONFIG_LCD_GC9A01_240X240
    #define DISPLAY_WIDTH   240
    #define DISPLAY_HEIGHT  240
#elif CONFIG_LCD_GC9A01_160X160
    #define DISPLAY_WIDTH   160
    #define DISPLAY_HEIGHT  160
#endif
//使用的SPI
#define DISPLAY_SPI1_NUM         (SPI3_HOST)
#define DISPLAY_SPI2_NUM         (SPI2_HOST)
//镜像和轴交换：通常不需要镜像或轴交换，除非你的硬件设计需要特定的显示方向。
#if CONFIG_LCD_GC9A01_240X240
    #define DISPLAY_MIRROR_X true
#elif CONFIG_LCD_GC9A01_160X160
    #define DISPLAY_MIRROR_X false
#endif

#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
//RGB顺序：LH128R-IG01使用RGB垂直条纹排列，因此设置为 LCD_RGB_ELEMENT_ORDER_RGB。
#if CONFIG_LCD_GC9A01_240X240
    #define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_BGR
#elif CONFIG_LCD_GC9A01_160X160
    #define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#endif
//偏移量：通常不需要偏移，除非你的显示屏有特定的显示区域限制。
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
//背光输出取反：默认不取反，除非你的背光控制电路需要取反信号。
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true
#if CONFIG_LCD_GC9A01_240X240
    #define DISPLAY_COLOR_INVERT true
#elif CONFIG_LCD_GC9A01_160X160
    #define DISPLAY_COLOR_INVERT false
#endif
/*==========小智+魔眼============ */
/* LCD settings */
#define GC9A01_LCD_PIXEL_CLK_HZ    (20 * 1000 * 1000)
#define GC9A01_LCD_CMD_BITS        (8)
#define GC9A01_LCD_PARAM_BITS      (8)
#define GC9A01_LCD_COLOR_SPACE     (ESP_LCD_COLOR_SPACE_RGB)
#define GC9A01_LCD_BITS_PER_PIXEL  (16)
// /* LCD-SPI2 pins */
// #define GC9A01_SPI2_LCD_GPIO_SCLK       (GPIO_NUM_38)
// #define GC9A01_SPI2_LCD_GPIO_MOSI       (GPIO_NUM_39)
// #define GC9A01_SPI2_LCD_GPIO_RST        (GPIO_NUM_40)
// #define GC9A01_SPI2_LCD_GPIO_DC         (GPIO_NUM_0)
// #define GC9A01_SPI2_LCD_GPIO_CS         (GPIO_NUM_NC)
// #define GC9A01_SPI2_LCD_GPIO_BL         (GPIO_NUM_NC)
// #define GC9A01_SPI2_LCD_GPIO_MISO       (GPIO_NUM_NC)

#define GC9A01_SPI1_LCD_GPIO_SCLK       (GPIO_NUM_4)
#define GC9A01_SPI1_LCD_GPIO_MOSI       (GPIO_NUM_5)
#define GC9A01_SPI1_LCD_GPIO_RST        (GPIO_NUM_6)
#define GC9A01_SPI1_LCD_GPIO_DC         (GPIO_NUM_3)
#define GC9A01_SPI1_LCD_GPIO_CS         (GPIO_NUM_2)
#define GC9A01_SPI1_LCD_GPIO_MISO       (GPIO_NUM_NC)

#if CONFIG_LCD_GC9A01_240X240
    #define GC9A01_SPI1_LCD_GPIO_BL         (GPIO_NUM_NC)
#elif CONFIG_LCD_GC9A01_160X160
    #define GC9A01_SPI1_LCD_GPIO_BL         (GPIO_NUM_1)
#endif 


#endif // _BOARD_CONFIG_H_
