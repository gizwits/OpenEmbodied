#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Movecall Moji configuration

#include <driver/gpio.h>
#include <lvgl.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE    true


#define AUDIO_CODEC_PA_PIN       GPIO_NUM_7
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_18
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_17
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// #define BUILTIN_LED_GPIO        GPIO_NUM_21
#define BOOT_BUTTON_GPIO        GPIO_NUM_21
#define POWER_GPIO        GPIO_NUM_14

// ST7789W3 240x296 配置
#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  296
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_SPI_MODE 0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_48

#define DISPLAY_SPI_SCLK_PIN    GPIO_NUM_6
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_15
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_5
#define DISPLAY_SPI_DC_PIN      GPIO_NUM_4
// #define DISPLAY_SPI_RESET_PIN   GPIO_NUM_NC
#define DISPLAY_SPI_RESET_PIN   GPIO_NUM_47

#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true


#define DISPLAY_SPI_SCLK_HZ     (20 * 1000 * 1000)

#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_WS GPIO_NUM_45
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_10

#define TOUCH_BUTTON_GPIO GPIO_NUM_46

#define CHARGING_PIN     GPIO_NUM_11   // CHRG pin
#define STANDBY_PIN      GPIO_NUM_3    // STDBY pin

#define BAT_ADC_CHANNEL  ADC_CHANNEL_1  // Battery voltage ADC channel
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11 // ADC attenuation
#define BAT_ADC_UNIT     ADC_UNIT_2
#define POWER_CHARGE_LED_PIN GPIO_NUM_NC


// 自动生成的螺旋图像数据
static const uint8_t qrcode_map[] =  {
   };

const lv_img_dsc_t qrcode_img = {
    .header = {
        .cf = LV_COLOR_FORMAT_RGB565,
        .w = 180,
        .h = 180,
    },
    .data_size = sizeof(qrcode_map),
    .data = qrcode_map,
};

#endif // _BOARD_CONFIG_H_
