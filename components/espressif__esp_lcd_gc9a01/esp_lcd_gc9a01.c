/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
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

#include "esp_lcd_gc9a01.h"

static const char *TAG = "gc9a01";

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel);
static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool off);

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
    const gc9a01_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} gc9a01_panel_t;

esp_err_t esp_lcd_new_panel_gc9a01(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    gc9a01_panel_t *gc9a01 = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    gc9a01 = (gc9a01_panel_t *)calloc(1, sizeof(gc9a01_panel_t));
    ESP_GOTO_ON_FALSE(gc9a01, ESP_ERR_NO_MEM, err, TAG, "no mem for gc9a01 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        gc9a01->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        gc9a01->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }
#else
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        gc9a01->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        gc9a01->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        gc9a01->colmod_val = 0x55;
        gc9a01->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        gc9a01->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        gc9a01->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    gc9a01->io = io;
    gc9a01->reset_gpio_num = panel_dev_config->reset_gpio_num;
    gc9a01->reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        gc9a01->init_cmds = ((gc9a01_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        gc9a01->init_cmds_size = ((gc9a01_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
    }
    gc9a01->base.del = panel_gc9a01_del;
    gc9a01->base.reset = panel_gc9a01_reset;
    gc9a01->base.init = panel_gc9a01_init;
    gc9a01->base.draw_bitmap = panel_gc9a01_draw_bitmap;
    gc9a01->base.invert_color = panel_gc9a01_invert_color;
    gc9a01->base.set_gap = panel_gc9a01_set_gap;
    gc9a01->base.mirror = panel_gc9a01_mirror;
    gc9a01->base.swap_xy = panel_gc9a01_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    gc9a01->base.disp_off = panel_gc9a01_disp_on_off;
#else
    gc9a01->base.disp_on_off = panel_gc9a01_disp_on_off;
#endif
    *ret_panel = &(gc9a01->base);
    ESP_LOGD(TAG, "new gc9a01 panel @%p", gc9a01);

    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_GC9A01_VER_MAJOR, ESP_LCD_GC9A01_VER_MINOR,
             ESP_LCD_GC9A01_VER_PATCH);

    return ESP_OK;

err:
    if (gc9a01) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(gc9a01);
    }
    return ret;
}

static esp_err_t panel_gc9a01_del(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);

    if (gc9a01->reset_gpio_num >= 0) {
        gpio_reset_pin(gc9a01->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del gc9a01 panel @%p", gc9a01);
    free(gc9a01);
    return ESP_OK;
}

static esp_err_t panel_gc9a01_reset(esp_lcd_panel_t *panel)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    // perform hardware reset
    if (gc9a01->reset_gpio_num >= 0) {
        gpio_set_level(gc9a01->reset_gpio_num, gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(gc9a01->reset_gpio_num, !gc9a01->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5ms before sending new command
    }

    return ESP_OK;
}

#if !CONFIG_LCD_GC9A01_160X160
static const gc9a01_lcd_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    // Enable Inter Register

    {0xfe, (uint8_t []){0x00}, 0, 0},
    {0xef, (uint8_t []){0x00}, 0, 0},
    {0xeb, (uint8_t []){0x14}, 1, 0},
    {0x84, (uint8_t []){0x60}, 1, 0},
    {0x85, (uint8_t []){0xff}, 1, 0},
    {0x86, (uint8_t []){0xff}, 1, 0},
    {0x87, (uint8_t []){0xff}, 1, 0},
    {0x8e, (uint8_t []){0xff}, 1, 0},
    {0x8f, (uint8_t []){0xff}, 1, 0},
    {0x88, (uint8_t []){0x0a}, 1, 0},
    {0x89, (uint8_t []){0x23}, 1, 0},
    {0x8a, (uint8_t []){0x00}, 1, 0},
    {0x8b, (uint8_t []){0x80}, 1, 0},
    {0x8c, (uint8_t []){0x01}, 1, 0},
    {0x8d, (uint8_t []){0x03}, 1, 0},
    {0x90, (uint8_t []){0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xff, (uint8_t []){0x60, 0x01, 0x04}, 3, 0},
    {0xC3, (uint8_t []){0x13}, 1, 0},
    {0xC4, (uint8_t []){0x13}, 1, 0},
    {0xC9, (uint8_t []){0x30}, 1, 0},
    {0xbe, (uint8_t []){0x11}, 1, 0},
    {0xe1, (uint8_t []){0x10, 0x0e}, 2, 0},
    {0xdf, (uint8_t []){0x21, 0x0c, 0x02}, 3, 0},
    // Set gamma
    {0xF0, (uint8_t []){0x45, 0x09, 0x08, 0x08, 0x26, 0x2a}, 6, 0},
    {0xF1, (uint8_t []){0x43, 0x70, 0x72, 0x36, 0x37, 0x6f}, 6, 0},
    {0xF2, (uint8_t []){0x45, 0x09, 0x08, 0x08, 0x26, 0x2a}, 6, 0},
    {0xF3, (uint8_t []){0x43, 0x70, 0x72, 0x36, 0x37, 0x6f}, 6, 0},
    {0xed, (uint8_t []){0x1b, 0x0b}, 2, 0},
    {0xae, (uint8_t []){0x77}, 1, 0},
    {0xcd, (uint8_t []){0x63}, 1, 0},
    {0x70, (uint8_t []){0x07, 0x07, 0x04, 0x0e, 0x0f, 0x09, 0x07, 0x08, 0x03}, 9, 0},
    {0xE8, (uint8_t []){0x34}, 1, 0}, // 4 dot inversion
    {0x60, (uint8_t []){0x38, 0x0b, 0x6D, 0x6D, 0x39, 0xf0, 0x6D, 0x6D}, 8, 0},
    {0x61, (uint8_t []){0x38, 0xf4, 0x6D, 0x6D, 0x38, 0xf7, 0x6D, 0x6D}, 8, 0},
    {0x62, (uint8_t []){0x38, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x38, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, (uint8_t []){0x38, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x38, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, (uint8_t []){0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},
    {0x66, (uint8_t []){0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, (uint8_t []){0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, (uint8_t []){0x10, 0x45, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},
    {0x98, (uint8_t []){0x3e, 0x07}, 2, 0},
    {0x99, (uint8_t []){0x3e, 0x07}, 2, 0},

};
    #endif

static esp_err_t panel_gc9a01_init(esp_lcd_panel_t *panel)
{
    #if !CONFIG_LCD_GC9A01_160X160
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9a01->madctl_val,
    }, 1), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        gc9a01->colmod_val,
    }, 1), TAG, "send command failed");

    const gc9a01_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    if (gc9a01->init_cmds) {
        init_cmds = gc9a01->init_cmds;
        init_cmds_size = gc9a01->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(gc9a01_lcd_init_cmd_t);
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            is_cmd_overwritten = true;
            gc9a01->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
            break;
        case LCD_CMD_COLMOD:
            is_cmd_overwritten = true;
            gc9a01->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
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
    #elif CONFIG_LCD_GC9A01_160X160
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    // 执行重置序列
    esp_lcd_panel_io_tx_param(io, 0xA0, NULL, 0);  // 重置
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_io_tx_param(io, 0xA0, NULL, 0);  // 重置
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_io_tx_param(io, 0xA0, NULL, 0);  // 重置
    vTaskDelay(pdMS_TO_TICKS(120));

    // 发送多组命令和数据
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xFE, NULL, 0), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xEF, NULL, 0), TAG, "send command failed");
    
    uint8_t data1[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x80, data1, sizeof(data1)), TAG, "send data failed");
    
    uint8_t data2[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x81, data2, sizeof(data2)), TAG, "send data failed");
    
    uint8_t data3[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x82, data3, sizeof(data3)), TAG, "send data failed");
    
    uint8_t data4[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x83, data4, sizeof(data4)), TAG, "send data failed");
    
    uint8_t data5[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x84, data5, sizeof(data5)), TAG, "send data failed");
    
    uint8_t data6[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x85, data6, sizeof(data6)), TAG, "send data failed");
    
    uint8_t data7[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x86, data7, sizeof(data7)), TAG, "send data failed");
    
    uint8_t data8[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x87, data8, sizeof(data8)), TAG, "send data failed");
    
    uint8_t data9[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x88, data9, sizeof(data9)), TAG, "send data failed");
    
    uint8_t dataA[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x89, dataA, sizeof(dataA)), TAG, "send data failed");
    
    uint8_t dataB[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x8A, dataB, sizeof(dataB)), TAG, "send data failed");
    
    uint8_t dataC[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x8B, dataC, sizeof(dataC)), TAG, "send data failed");
    
    uint8_t dataD[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x8C, dataD, sizeof(dataD)), TAG, "send data failed");
    
    uint8_t dataE[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x8D, dataE, sizeof(dataE)), TAG, "send data failed");
    
    uint8_t dataF[] = {0xFF};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x8E, dataF, sizeof(dataF)), TAG, "send data failed");
    
    uint8_t data10[] = {0x05};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3A, data10, sizeof(data10)), TAG, "send data failed");
    
    uint8_t data11[] = {0x01};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xEC, data11, sizeof(data11)), TAG, "send data failed");
    
    uint8_t data12[] = {0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x74, data12, sizeof(data12)), TAG, "send data failed");
    
    uint8_t data13[] = {0x3E};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x98, data13, sizeof(data13)), TAG, "send data failed");
    
    uint8_t data14[] = {0x3E};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x99, data14, sizeof(data14)), TAG, "send data failed");
    
    uint8_t data15[] = {0x0D, 0x0D};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB5, data15, sizeof(data15)), TAG, "send data failed");
    
    uint8_t data16[] = {0x38, 0x0F, 0x79, 0x67};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x60, data16, sizeof(data16)), TAG, "send data failed");
    
    uint8_t data17[] = {0x38, 0x11, 0x79, 0x67};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x61, data17, sizeof(data17)), TAG, "send data failed");
    
    uint8_t data18[] = {0x38, 0x17, 0x71, 0x5F, 0x79, 0x67};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x64, data18, sizeof(data18)), TAG, "send data failed");
    
    uint8_t data19[] = {0x38, 0x13, 0x71, 0x5B, 0x79, 0x67};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x65, data19, sizeof(data19)), TAG, "send data failed");
    
    uint8_t data20[] = {0x00, 0x00};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x6A, data20, sizeof(data20)), TAG, "send data failed");
    
    uint8_t data21[] = {0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x6C, data21, sizeof(data21)), TAG, "send data failed");
    
    uint8_t data22[] = {0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0f, 0x0f, 0x0d, 0x0d, 0x0b, 0x0b, 0x09, 0x09, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x0a, 0x0c, 0x0c, 0x0e, 0x0e, 0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x6E, data22, sizeof(data22)), TAG, "send data failed");
    
    uint8_t data23[] = {0x01};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xBF, data23, sizeof(data23)), TAG, "send data failed");
    
    uint8_t data24[] = {0x40};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF9, data24, sizeof(data24)), TAG, "send data failed");
    
    uint8_t data25[] = {0x3b, 0x33, 0x7f, 0x00};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x9b, data25, sizeof(data25)), TAG, "send data failed");
    
    uint8_t data26[] = {0x30};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x7E, data26, sizeof(data26)), TAG, "send data failed");
    
    uint8_t data27[] = {0x0d, 0x02, 0x08, 0x0d, 0x02, 0x08};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x70, data27, sizeof(data27)), TAG, "send data failed");
    
    uint8_t data28[] = {0x0d, 0x02, 0x08};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x71, data28, sizeof(data28)), TAG, "send data failed");
    
    uint8_t data29[] = {0x0E, 0x09};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x91, data29, sizeof(data29)), TAG, "send data failed");
    
    uint8_t data30[] = {0x18};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC3, data30, sizeof(data30)), TAG, "send data failed");
    
    uint8_t data31[] = {0x18};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC4, data31, sizeof(data31)), TAG, "send data failed");
    
    uint8_t data32[] = {0x3c};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC9, data32, sizeof(data32)), TAG, "send data failed");
    
    uint8_t data33[] = {0x13, 0x15, 0x04, 0x05, 0x01, 0x38};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF0, data33, sizeof(data33)), TAG, "send data failed");
    
    uint8_t data34[] = {0x13, 0x15, 0x04, 0x05, 0x01, 0x34};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF2, data34, sizeof(data34)), TAG, "send data failed");
    
    uint8_t data35[] = {0x4b, 0xb8, 0x7b, 0x34, 0x35, 0xef};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF1, data35, sizeof(data35)), TAG, "send data failed");
    
    uint8_t data36[] = {0x47, 0xb4, 0x72, 0x34, 0x35, 0xda};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF3, data36, sizeof(data36)), TAG, "send data failed");
    
    uint8_t data37[] = {0x00};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x36, data37, sizeof(data37)), TAG, "send data failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG, "send command failed");
    vTaskDelay(pdMS_TO_TICKS(200)); 
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x2C, NULL, 0), TAG, "send command failed");

    return ESP_OK;
    #endif
}

static esp_err_t panel_gc9a01_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = gc9a01->io;

    x_start += gc9a01->x_gap;
    x_end += gc9a01->x_gap;
    y_start += gc9a01->y_gap;
    y_end += gc9a01->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * gc9a01->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "send color failed");

    return ESP_OK;
}

static esp_err_t panel_gc9a01_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9a01_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    if (mirror_x) {
        gc9a01->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        gc9a01->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9a01->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9a01_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    if (swap_axes) {
        gc9a01->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        gc9a01->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        gc9a01->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_gc9a01_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    gc9a01->x_gap = x_gap;
    gc9a01->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_gc9a01_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    gc9a01_panel_t *gc9a01 = __containerof(panel, gc9a01_panel_t, base);
    esp_lcd_panel_io_handle_t io = gc9a01->io;
    int command = 0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
