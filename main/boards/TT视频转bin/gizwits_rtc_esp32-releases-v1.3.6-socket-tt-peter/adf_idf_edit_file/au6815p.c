#include <stdint.h>
#include "au6815p_reg.h"
#include <stdio.h>
#include "driver/i2c.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "au6815p.h"
#include "audio_hal.h"
#include "board_pins_config.h"
#include "i2c_bus.h"
#include "audio_volume.h"
#include <math.h>

#define TAG "AU6815P"

#define AU6815P_RST_GPIO            get_pa_enable_gpio()
#define AU6815P_I2C_NUM             I2C_NUM_0 /*!< I2C port number for master dev */
#define AU6815P_ADDR                0x58  /*!< Slave address for AU6815P */

#define AU6815P_ASSERT(a, format, b, ...) \
    if ((a) != 0) { \
        ESP_LOGE(TAG, format, ##__VA_ARGS__); \
        return b;\
    }

static i2c_bus_handle_t     i2c_handler;
static codec_dac_volume_config_t *dac_vol_handle;

// esp_err_t au6815_i2c_master_init(void) {
//     ESP_LOGI(TAG, "Initializing I2C master...");
//     i2c_master_bus_config_t bus_config = {
//         .i2c_port = AU6815P_I2C_NUM,
//         .sda_io_num = I2C_MASTER_SDA_IO,
//         .scl_io_num = I2C_MASTER_SCL_IO,
//         .clk_source = I2C_CLK_SRC_DEFAULT,
//         .glitch_ignore_cnt = 7,
//         // .intr_priority = 1,
//         // .trans_queue_depth = 10,
//         .flags.enable_internal_pullup = 1
//     };
//     esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
//     if (ret == ESP_OK) {
//         ESP_LOGI(TAG, "I2C master initialized successfully");
//     } else {
//         ESP_LOGE(TAG, "Failed to initialize I2C master: %s", esp_err_to_name(ret));
//     }
//     return ret;
// }

// static esp_err_t au6815p_write_register(uint8_t reg, uint8_t value) {
//     i2c_master_dev_handle_t dev_handle;
//     i2c_device_config_t dev_config = {
//         .dev_addr_length = I2C_ADDR_BIT_LEN_7,
//         .device_address = AU6815P_ADDR,
//         .scl_speed_hz = I2C_MASTER_FREQ_HZ,
//         .scl_wait_us = 0,
//         .flags.disable_ack_check = 0
//     };
//     esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
//         return ret;
//     }
//     uint8_t data[2] = {reg, value};
//     ret = i2c_master_transmit(dev_handle, data, 2, I2C_MASTER_TIMEOUT_MS);
//     if (ret == ESP_OK) {
//         // ESP_LOGI(TAG, "Successfully wrote to register 0x%02X with value 0x%02X", reg, value);
//     } else {
//         ESP_LOGE(TAG, "Failed to write to register 0x%02X with value 0x%02X: %s", reg, value, esp_err_to_name(ret));
//     }
//     return ret;
// }

// void au6815p_init(void) {
//     ESP_LOGI(TAG, "Initializing AU6815P in...");
//     // for (int i = 1; i < 2; i++) {
//     for (int i = 0; i < sizeof(au6815_reg) / sizeof(au6815_reg[0]); i++) {
//         // ESP_LOGI(TAG, "Writing to [%d] register 0x%02X with value 0x%02X", i, au6815_reg[i].reg, au6815_reg[i].value);
//         au6815p_write_register(au6815_reg[i].reg, au6815_reg[i].value);
//     }
//     ESP_LOGI(TAG, "Initializing AU6815P out...");
// }
/*
 * Operate fuction of PA
 */

/*
 * i2c default configuration
 */
static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000,
};

audio_hal_func_t AUDIO_CODEC_AU6815P_DEFAULT_HANDLE = {
    .audio_codec_initialize = au6815p_init,
    .audio_codec_deinitialize = au6815p_deinit,
    .audio_codec_ctrl = au6815p_ctrl,
    .audio_codec_config_iface = au6815p_config_iface,
    .audio_codec_set_mute = au6815p_set_mute,
    .audio_codec_set_volume = au6815p_set_volume,
    .audio_codec_get_volume = au6815p_get_volume,
    .audio_codec_enable_pa = NULL,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

static esp_err_t au6815p_transmit_registers(const cfg_reg_t *conf_buf, int size)
{
    int i = 0;
    esp_err_t ret = ESP_OK;
    while (i < size) {
        switch (conf_buf[i].offset) {
            case CFG_META_SWITCH:
                // Used in legacy applications.  Ignored here.
                break;
            case CFG_META_DELAY:
                vTaskDelay(conf_buf[i].value / portTICK_RATE_MS);
                break;
            case CFG_META_BURST:
                ret = i2c_bus_write_bytes(i2c_handler, AU6815P_ADDR, (unsigned char *)(&conf_buf[i + 1].offset), 1, (unsigned char *)(&conf_buf[i + 1].value), conf_buf[i].value);
                i +=  (conf_buf[i].value / 2) + 1;
                break;
            default:
                ret = i2c_bus_write_bytes(i2c_handler, AU6815P_ADDR, (unsigned char *)(&conf_buf[i].offset), 1, (unsigned char *)(&conf_buf[i].value), 1);
                break;
        }
        i++;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fail to load configuration to tas5805m");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s:  write %d reg done", __FUNCTION__, i);
    return ret;
}

esp_err_t au6815p_init(audio_hal_codec_config_t *codec_cfg)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGW(TAG, "Power ON CODEC with GPIO %d", AU6815P_RST_GPIO);
    gpio_config_t io_conf;
    io_conf.pin_bit_mask = BIT64(AU6815P_RST_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(AU6815P_RST_GPIO, 0);
    vTaskDelay(50 / portTICK_RATE_MS);
    gpio_set_level(AU6815P_RST_GPIO, 1);
    vTaskDelay(100 / portTICK_RATE_MS);

    ret = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
    i2c_handler = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
    if (i2c_handler == NULL) {
        ESP_LOGW(TAG, "failed to create i2c bus handler\n");
        return ESP_FAIL;
    }

    ret |= au6815p_transmit_registers(au6815p_reg, sizeof(au6815p_reg) / sizeof(au6815p_reg[0]));

    AU6815P_ASSERT(ret, "Fail to iniitialize au6815p PA", ESP_FAIL);

    // codec_dac_volume_config_t vol_cfg = AU6815P_DAC_VOL_CFG_DEFAULT();
    // dac_vol_handle = audio_codec_volume_init(&vol_cfg);

    return ret;
}

esp_err_t au6815p_deinit(void)
{
    // TODO
    i2c_bus_delete(i2c_handler);
    // audio_codec_volume_deinit(dac_vol_handle);
    return ESP_OK;
}

esp_err_t au6815p_set_mute(bool enable)
{
    esp_err_t ret = ESP_OK;
    
    if (enable) {
        ESP_LOGI(TAG, "Muting AU6815P...");
        ret = au6815p_transmit_registers(mute_function, sizeof(mute_function) / sizeof(mute_function[0]));
        ESP_LOGI(TAG, "AU6815P muted");
    } else {
        ESP_LOGI(TAG, "Starting AU6815P play function...");
        ret = au6815p_transmit_registers(play_function, sizeof(play_function) / sizeof(play_function[0]));
        ESP_LOGI(TAG, "AU6815P play function started");
    }

    return ret;
}


esp_err_t au6815p_ctrl(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    ESP_LOGI(TAG, "AU6815P ctrl: %d, %d", mode, ctrl_state);
    if (ctrl_state == AUDIO_HAL_CTRL_START) {
        ESP_LOGI(TAG, "AU6815P AUDIO_HAL_CTRL_START: %d, %d", mode, ctrl_state);
        au6815p_set_mute(false);
    } else {
        // TODO
        ESP_LOGI(TAG, "AU6815P ctrl not implemented: %d, %d", mode, ctrl_state);
    }
    return ESP_OK;
}

esp_err_t au6815p_config_iface(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    ESP_LOGI(TAG, "AU6815P config iface not implemented: %d, %p", mode, iface);
    //TODO
    return ESP_OK;
}

void au6815p_deepsleep(void) {
    ESP_LOGI(TAG, "Putting AU6815P into deep sleep...");
    au6815p_transmit_registers(deepsleep_function, sizeof(deepsleep_function) / sizeof(deepsleep_function[0]));
    ESP_LOGI(TAG, "AU6815P is now in deep sleep");
}

void au6815p_power_down(void) {
    ESP_LOGI(TAG, "Powering down AU6815P...");
    au6815p_transmit_registers(power_down_steps, sizeof(power_down_steps) / sizeof(power_down_steps[0]));
    ESP_LOGI(TAG, "AU6815P powered down");
}


/**
 * @brief Set voice volume
 *
 * @note Register values. 0xFE: -103 dB, 0x94: -50 dB, 0x30: 0 dB, 0x00: 24 dB
 * @note Accuracy of gain is 0.5 dB
 *
 * @param volume: voice volume (0~100)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t au6815p_set_volume(int volume)
{
    esp_err_t ret = ESP_OK;
    ESP_LOGI(TAG, "Setting AU6815P volume to %d...", volume);
    
    // 设置音量前的操作步骤
    ret = au6815p_transmit_registers(volume_set_steps_pre, sizeof(volume_set_steps_pre) / sizeof(volume_set_steps_pre[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume pre steps");
        return ret;
    }

    // 将0-100的音量转换为-60dB到-20dB
    // float dB = -50.0 + (volume / 100.0) * 50.0;
    // float dB = -60.0 + (volume / 100.0) * 40.0;
    // float dB = -59.0 + (volume / 100.0) * 40.0;
    float dB = -39.0 + (volume / 100.0) * 20.0 + 15;
    ESP_LOGW(TAG, "AU6815P volume: %d, dB: %.2f", volume, dB);

    // 计算目标寄存器值
    uint32_t reg_value = (uint32_t)(8388608 * pow(10, dB / 20));
    
    // 将寄存器值分解为4个字节并创建配置数组
    cfg_reg_t volume_regs[] = {
        { 0x24, (reg_value >> 24) & 0xFF },
        { 0x25, (reg_value >> 16) & 0xFF },
        { 0x26, (reg_value >> 8) & 0xFF },
        { 0x27, reg_value & 0xFF },
        { 0x28, (reg_value >> 24) & 0xFF },
        { 0x29, (reg_value >> 16) & 0xFF },
        { 0x2A, (reg_value >> 8) & 0xFF },
        { 0x2B, reg_value & 0xFF }
    };

    // 使用transmit_registers写入所有寄存器
    ret = au6815p_transmit_registers(volume_regs, sizeof(volume_regs) / sizeof(volume_regs[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write volume registers");
        return ret;
    }

    // 设置音量后的操作步骤
    ret = au6815p_transmit_registers(volume_set_steps_post, sizeof(volume_set_steps_post) / sizeof(volume_set_steps_post[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume post steps");
        return ret;
    }

    ESP_LOGI(TAG, "AU6815P volume set success");
    return ret;
}

esp_err_t au6815p_get_volume(int *volume)
{
    /// FIXME: Got the digit volume is not right.
    // uint8_t cmd[2] = {MASTER_VOL_REG_ADDR, 0x00};
    // esp_err_t ret = i2c_bus_read_bytes(i2c_handler, TAS5805M_ADDR, &cmd[0], 1, &cmd[1], 1);
    // TAS5805M_ASSERT(ret, "Fail to get volume", ESP_FAIL);
    // if (cmd[1] == dac_vol_handle->reg_value) {
    //     *volume = dac_vol_handle->user_volume;
    // } else {
    //     *volume = 0;
    //     ret = ESP_FAIL;
    // }
    // ESP_LOGD(TAG, "Get volume:%.2d reg_value:0x%.2x", *volume, cmd[1]);
    // return ret;
    *volume = AUDIO_HAL_VOL_DEFAULT;
    return ESP_OK;
}

// esp_err_t tas5805m_get_mute(int *value)
// {
//     esp_err_t ret = ESP_OK;
//     uint8_t cmd[2] = {TAS5805M_REG_03, 0x00};
//     ret |= i2c_bus_read_bytes(i2c_handler, TAS5805M_ADDR, &cmd[0], 1, &cmd[1], 1);

//     TAS5805M_ASSERT(ret, "Fail to get mute", ESP_FAIL);
//     *value = (cmd[1] & 0x08) >> 4;
//     ESP_LOGI(TAG, "Get mute value: 0x%x", *value);
//     return ret;
// }

// esp_err_t tas5805m_set_mute_fade(int value)
// {
//     esp_err_t ret = 0;
//     unsigned char cmd[2] = {MUTE_TIME_REG_ADDR, 0x00};
//     /* Time for register value
//     *   000: 11.5 ms
//     *   001: 53 ms
//     *   010: 106.5 ms
//     *   011: 266.5 ms
//     *   100: 0.535 sec
//     *   101: 1.065 sec
//     *   110: 2.665 sec
//     *   111: 5.33 sec
//     */
//     if (value <= 12) {
//         cmd[1] = 0;
//     } else if (value <= 53) {
//         cmd[1] = 1;
//     } else if (value <= 107) {
//         cmd[1] = 2;
//     } else if (value <= 267) {
//         cmd[1] = 3;
//     } else if (value <= 535) {
//         cmd[1] = 4;
//     } else if (value <= 1065) {
//         cmd[1] = 5;
//     } else if (value <= 2665) {
//         cmd[1] = 6;
//     } else {
//         cmd[1] = 7;
//     }
//     cmd[1] |= (cmd[1] << 4);

//     ret |= i2c_bus_write_bytes(i2c_handler, TAS5805M_ADDR, &cmd[0], 1, &cmd[1], 1);
//     TAS5805M_ASSERT(ret, "Fail to set mute fade", ESP_FAIL);
//     ESP_LOGI(TAG, "Set mute fade, value:%d, 0x%x", value, cmd[1]);
//     return ret;
// }
