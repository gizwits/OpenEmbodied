/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_lcd_st7735s.h"

static const char *TAG = "st7735s";

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const st7735s_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} st7735s_panel_t;

esp_err_t esp_lcd_new_panel_st7735s(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    st7735s_panel_t *st7735s = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7735s = (st7735s_panel_t *)calloc(1, sizeof(st7735s_panel_t));
    ESP_GOTO_ON_FALSE(st7735s, ESP_ERR_NO_MEM, err, TAG, "no mem for st7735s panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        st7735s->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        st7735s->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }
#else
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        st7735s->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        st7735s->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7735s->colmod_val = 0x05; // 16-bit color
        st7735s->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st7735s->colmod_val = 0x06; // 18-bit color
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        st7735s->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7735s->io = io;
    st7735s->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7735s->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        st7735s->init_cmds = ((st7735s_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        st7735s->init_cmds_size = ((st7735s_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
    }
    st7735s->base.del = panel_st7735s_del;
    st7735s->base.reset = panel_st7735s_reset;
    st7735s->base.init = panel_st7735s_init;
    st7735s->base.draw_bitmap = panel_st7735s_draw_bitmap;
    st7735s->base.invert_color = panel_st7735s_invert_color;
    st7735s->base.set_gap = panel_st7735s_set_gap;
    st7735s->base.mirror = panel_st7735s_mirror;
    st7735s->base.swap_xy = panel_st7735s_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    st7735s->base.disp_off = panel_st7735s_disp_on_off;
#else
    st7735s->base.disp_on_off = panel_st7735s_disp_on_off;
#endif
    *ret_panel = &(st7735s->base);
    ESP_LOGD(TAG, "new st7735s panel @%p", st7735s);

    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_ST7735S_VER_MAJOR, ESP_LCD_ST7735S_VER_MINOR,
             ESP_LCD_ST7735S_VER_PATCH);

    return ESP_OK;

err:
    if (st7735s) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7735s);
    }
    return ret;
}

static esp_err_t panel_st7735s_del(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);

    if (st7735s->reset_gpio_num >= 0) {
        gpio_reset_pin(st7735s->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7735s panel @%p", st7735s);
    free(st7735s);
    return ESP_OK;
}

static esp_err_t panel_st7735s_reset(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;

    // perform hardware reset
    if (st7735s->reset_gpio_num >= 0) {
        gpio_set_level(st7735s->reset_gpio_num, st7735s->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7735s->reset_gpio_num, !st7735s->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5ms before sending new command
    }

    return ESP_OK;
}

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes; // Length of data in above data array; 0xFF = end of cmds.
} lcd_init_cmd_t;

// ST7735S specific commands used in init
#define ST7735_NOP          0x00
#define ST7735_SWRESET      0x01
#define ST7735_RDDID        0x04
#define ST7735_RDDST        0x09

#define ST7735_SLPIN        0x10
#define ST7735_SLPOUT       0x11
#define ST7735_PTLON        0x12
#define ST7735_NORON        0x13

#define ST7735_INVOFF       0x20
#define ST7735_INVON        0x21
#define ST7735_DISPOFF      0x28
#define ST7735_DISPON       0x29
#define ST7735_CASET        0x2A
#define ST7735_RASET        0x2B
#define ST7735_RAMWR        0x2C
#define ST7735_RAMRD        0x2E

#define ST7735_PTLAR        0x30
#define ST7735_VSCRDEF      0x33
#define ST7735_COLMOD       0x3A
#define ST7735_MADCTL       0x36
#define ST7735_VSCRSADD     0x37

#define ST7735_FRMCTR1      0xB1
#define ST7735_FRMCTR2      0xB2
#define ST7735_FRMCTR3      0xB3
#define ST7735_INVCTR       0xB4
#define ST7735_DISSET5      0xB6

#define ST7735_PWCTR1       0xC0
#define ST7735_PWCTR2       0xC1
#define ST7735_PWCTR3       0xC2
#define ST7735_PWCTR4       0xC3
#define ST7735_PWCTR5       0xC4
#define ST7735_VMCTR1       0xC5

#define ST7735_RDID1        0xDA
#define ST7735_RDID2        0xDB
#define ST7735_RDID3        0xDC
#define ST7735_RDID4        0xDD

#define ST7735_PWCTR6       0xFC

#define ST7735_GMCTRP1      0xE0
#define ST7735_GMCTRN1      0xE1

// For ST7735S TFT (128x160) - no offset needed
#define ST7735_REDTAB128x160
#define COLSTART            0
#define ROWSTART            0

// Display dimensions for ST7735S TFT
#define LCD_WIDTH           128
#define LCD_HEIGHT          160

// Delay between some initialisation commands
#define TFT_INIT_DELAY      0x80

// Static data arrays for initialization commands
static const uint8_t init_data_0[] = {0};
static const uint8_t init_data_1[] = {0};
static const uint8_t init_data_2[] = {0x01, 0x2C, 0x2D};
static const uint8_t init_data_3[] = {0x01, 0x2C, 0x2D};
static const uint8_t init_data_4[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
static const uint8_t init_data_5[] = {0x07};
static const uint8_t init_data_6[] = {0xA2, 0x02, 0x84};
static const uint8_t init_data_7[] = {0xC5};
static const uint8_t init_data_8[] = {0x0A, 0x00};
static const uint8_t init_data_9[] = {0x8A, 0x2A};
static const uint8_t init_data_10[] = {0x8A, 0xEE};
static const uint8_t init_data_11[] = {0x0E};
static const uint8_t init_data_12[] = {0};
static const uint8_t init_data_13[] = {0x00};
static const uint8_t init_data_14[] = {0x05};
static const uint8_t init_data_15[] = {0x0F, 0x1A, 0x0F, 0x18, 0x2F, 0x28, 0x20, 0x22, 0x1F, 0x1B, 0x23, 0x37, 0x00, 0x07, 0x02, 0x10};
static const uint8_t init_data_16[] = {0x0F, 0x1B, 0x0F, 0x17, 0x33, 0x2C, 0x29, 0x2E, 0x30, 0x30, 0x39, 0x3F, 0x00, 0x07, 0x03, 0x10};
static const uint8_t init_data_17[] = {0};
static const uint8_t init_data_18[] = {0};
static const uint8_t init_data_19[] = {0};

static const st7735s_lcd_init_cmd_t vendor_specific_init_default[] = {
    {ST7735_SWRESET, init_data_0, 0, 150},                 // Software reset, 0 args, w/delay 150us
    {ST7735_SLPOUT, init_data_1, 0, 500},                  // Sleep out, 0 args, w/delay 500us
    {ST7735_FRMCTR1, init_data_2, 3, 0},                   // Frame rate ctrl - fastest refresh, 3 args
    {ST7735_FRMCTR2, init_data_3, 3, 0},                   // Frame rate control - idle mode, 3 args
    {ST7735_FRMCTR3, init_data_4, 6, 0},                   // Frame rate ctrl - partial mode, 6 args
    {ST7735_INVCTR, init_data_5, 1, 0},                    // Display inversion ctrl, 1 arg, no delay:Line inversion
    {ST7735_PWCTR1, init_data_6, 3, 0},                    // Power control, 3 args, no delay
    {ST7735_PWCTR2, init_data_7, 1, 0},                    // Power control, 1 arg, no delay:VGH = 14.7V, VGL = -7.35V
    {ST7735_PWCTR3, init_data_8, 2, 0},                    // Power control, 2 args, no delay:Opamp current small, Boost frequency
    {ST7735_PWCTR4, init_data_9, 2, 0},                    // Power control, 2 args, no delay:Opamp current small, Boost frequency
    {ST7735_PWCTR5, init_data_10, 2, 0},                   // Power control, 2 args, no delay:Opamp current small, Boost frequency
    {ST7735_VMCTR1, init_data_11, 1, 0},                   // Power control, 1 arg, no delay
    {ST7735_INVOFF, init_data_12, 0, 0},                   // Display inversion off
    {ST7735_MADCTL, init_data_13, 1, 0},                   // Memory access control, 1 arg, no delay
    {ST7735_COLMOD, init_data_14, 1, 0},                   // set color mode, 1 arg, no delay: 16-bit color
    {ST7735_GMCTRP1, init_data_15, 16, 0},                 // Positive gamma correction
    {ST7735_GMCTRN1, init_data_16, 16, 0},                 // Negative gamma correction
    {ST7735_DISPON, init_data_17, 0, 100},                 // Main screen turn on, no args, w/delay 100us
    {ST7735_NORON, init_data_18, 0, 10},                   // Normal display on, no args, w/delay 10us
    {0, init_data_19, 0, 0xff}
};

static esp_err_t panel_st7735s_init(esp_lcd_panel_t *panel)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;

    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_MADCTL, (uint8_t[]) {
        st7735s->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_COLMOD, (uint8_t[]) {
        st7735s->colmod_val,
    }, 1), TAG, "send command failed");

    const st7735s_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    if (st7735s->init_cmds) {
        init_cmds = st7735s->init_cmds;
        init_cmds_size = st7735s->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(st7735s_lcd_init_cmd_t);
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case ST7735_MADCTL:
            is_cmd_overwritten = true;
            st7735s->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case ST7735_COLMOD:
            is_cmd_overwritten = true;
            st7735s->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_st7735s_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st7735s->io;

    x_start += st7735s->x_gap;
    x_end += st7735s->x_gap;
    y_start += st7735s->y_gap;
    y_end += st7735s->y_gap;

    // define an area of frame memory where MCU can access
    // For 0.96 inch TFT, add offset for proper positioning
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        (x_start + COLSTART) & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1 + COLSTART) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        (y_start + ROWSTART) & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end -1 + ROWSTART) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7735s->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_st7735s_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;
    int command = 0;
    if (invert_color_data) {
        command = ST7735_INVON;
    } else {
        command = ST7735_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;
    if (mirror_x) {
        st7735s->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7735s->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7735s->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7735s->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_MADCTL, (uint8_t[]) {
        st7735s->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;
    if (swap_axes) {
        st7735s->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7735s->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7735_MADCTL, (uint8_t[]) {
        st7735s->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_st7735s_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    st7735s->x_gap = x_gap;
    st7735s->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7735s_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7735s_panel_t *st7735s = __containerof(panel, st7735s_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7735s->io;
    int command = 0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    if (on_off) {
        command = ST7735_DISPON;
    } else {
        command = ST7735_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
