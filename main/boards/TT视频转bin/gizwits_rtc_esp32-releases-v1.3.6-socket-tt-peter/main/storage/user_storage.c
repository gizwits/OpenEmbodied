#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "user_storage.h"
#include "freertos/FreeRTOS.h"


static const char *TAG = "USER_STORAGE";

// NVS 命名空间
#define STORAGE_NAMESPACE "local_data"

// 存储键名
#define RGB_RED_KEY "rgb_red"
#define RGB_GREEN_KEY "rgb_green"
#define RGB_BLUE_KEY "rgb_blue"
#define RGB_BRIGHTNESS_KEY "rgb_bright"
#define VOLUME_KEY "volume"

/**
 * 初始化存储系统
 * 
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区被格式化，重新初始化
        ESP_LOGI(TAG, "Erasing NVS flash...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "User storage initialized successfully");
    }
    
    return err;
}

/**
 * 保存 RGB 颜色值
 * 
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_rgb_color(uint8_t r, uint8_t g, uint8_t b)
{
    nvs_handle_t handle;
    esp_err_t err;
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // 写入 RGB 颜色分量
    err = nvs_set_u8(handle, RGB_RED_KEY, r);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving red component: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    
    err = nvs_set_u8(handle, RGB_GREEN_KEY, g);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving green component: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    
    err = nvs_set_u8(handle, RGB_BLUE_KEY, b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving blue component: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    
    // 提交更改
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "RGB color (%d, %d, %d) saved successfully", r, g, b);
    }
    
    // 关闭句柄
    nvs_close(handle);
    return err;
}

/**
 * 读取 RGB 颜色值
 * 
 * @param r 指向存储红色分量的变量的指针
 * @param g 指向存储绿色分量的变量的指针
 * @param b 指向存储蓝色分量的变量的指针
 * @param default_r 默认红色分量
 * @param default_g 默认绿色分量
 * @param default_b 默认蓝色分量
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_rgb_color(uint8_t *r, uint8_t *g, uint8_t *b, 
                                     uint8_t default_r, uint8_t default_g, uint8_t default_b)
{
    if (r == NULL || g == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t handle;
    esp_err_t err;
    bool using_defaults = false;
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        *r = default_r;
        *g = default_g;
        *b = default_b;
        return err;
    }
    
    // 读取红色分量
    err = nvs_get_u8(handle, RGB_RED_KEY, r);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *r = default_r;
        using_defaults = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading red component: %s", esp_err_to_name(err));
        *r = default_r;
    }
    
    // 读取绿色分量
    err = nvs_get_u8(handle, RGB_GREEN_KEY, g);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *g = default_g;
        using_defaults = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading green component: %s", esp_err_to_name(err));
        *g = default_g;
    }
    
    // 读取蓝色分量
    err = nvs_get_u8(handle, RGB_BLUE_KEY, b);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *b = default_b;
        using_defaults = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading blue component: %s", esp_err_to_name(err));
        *b = default_b;
    }
    
    // 关闭句柄
    nvs_close(handle);
    
    if (using_defaults) {
        ESP_LOGI(TAG, "Some RGB components not found, using defaults: (%d, %d, %d)", *r, *g, *b);
    } else {
        ESP_LOGI(TAG, "Loaded RGB color: (%d, %d, %d)", *r, *g, *b);
    }
    
    return ESP_OK;
}

/**
 * 保存 RGB 亮度值
 * 
 * @param brightness 亮度值 (0-100)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_brightness(uint8_t brightness)
{
    nvs_handle_t handle;
    esp_err_t err;
    
    // 确保亮度在有效范围内
    if (brightness > 100) {
        brightness = 100;
    }
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    
    // 写入亮度值
    err = nvs_set_u8(handle, RGB_BRIGHTNESS_KEY, brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving brightness: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }
    
    // 提交更改
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Brightness %d%% saved successfully", brightness);
    }
    
    // 关闭句柄
    nvs_close(handle);
    return err;
}

/**
 * 读取 RGB 亮度值
 * 
 * @param brightness 指向存储亮度值的变量的指针
 * @param default_brightness 如果没有存储的值，使用的默认亮度
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_brightness(uint8_t *brightness, uint8_t default_brightness)
{
    if (brightness == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t handle;
    esp_err_t err;
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        *brightness = default_brightness;
        return err;
    }
    
    // 读取亮度值
    err = nvs_get_u8(handle, RGB_BRIGHTNESS_KEY, brightness);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Brightness not found, using default: %d%%", default_brightness);
        *brightness = default_brightness;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading brightness: %s", esp_err_to_name(err));
        *brightness = default_brightness;
    } else {
        ESP_LOGI(TAG, "Loaded brightness: %d%%", *brightness);
    }
    
    // 关闭句柄
    nvs_close(handle);
    return err;
}

/**
 * 保存音量值
 * 
 * @param volume 音量值 (0-100)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_volume(uint8_t volume)
{
    static uint8_t last_volume = 0xff;
    nvs_handle_t handle;
    esp_err_t err;
    
    if(last_volume == volume)
    {
        return ESP_OK;
    }
    set_manual_break_flag(true);

    printf("%s, %d\n", __func__, __LINE__);

    // 确保音量在有效范围内
    if (volume > 100) {
        volume = 100;
    }
    printf("%s, %d\n", __func__, __LINE__);
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }
    printf("%s, %d\n", __func__, __LINE__);
    
    while (get_ai_is_playing() || get_audio_url_is_playing() || audio_tone_url_is_playing()\
        || get_is_playing_cache() || !get_i2s_is_finished()|| !get_url_i2s_is_finished()) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            // ESP_LOGE(TAG, "wait for volume to save, audio is playing...");
    }
    // 当URL在播，互斥NVS和i2s音频管道操作
    set_audio_url_player_state();
    printf("%s, %d\n", __func__, __LINE__);
    // 写入音量值
    err = nvs_set_u8(handle, VOLUME_KEY, volume);
    printf("%s, %d\n", __func__, __LINE__);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving volume: %s", esp_err_to_name(err));
    printf("%s, %d\n", __func__, __LINE__);
        nvs_close(handle);
        return err;
    }
    printf("%s, %d\n", __func__, __LINE__);
    
    // 提交更改
    err = nvs_commit(handle);
    printf("%s, %d\n", __func__, __LINE__);
    if (err != ESP_OK) {
    printf("%s, %d\n", __func__, __LINE__);
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Volume %d%% saved successfully", volume);
    printf("%s, %d\n", __func__, __LINE__);
        last_volume = volume;
    }
    printf("%s, %d\n", __func__, __LINE__);
    
    // 关闭句柄
    nvs_close(handle);
    printf("%s, %d\n", __func__, __LINE__);
    vTaskDelay(200 / portTICK_PERIOD_MS);
    printf("%s, %d\n", __func__, __LINE__);
    set_manual_break_flag(false);
    reset_audio_url_player_state();
    return err;
}

/**
 * 读取音量值
 * 
 * @param volume 指向存储音量值的变量的指针
 * @param default_volume 如果没有存储的值，使用的默认音量
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_volume(uint8_t *volume, uint8_t default_volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t handle;
    esp_err_t err;
    
    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        *volume = default_volume;
        return err;
    }
    
    // 读取音量值
    err = nvs_get_u8(handle, VOLUME_KEY, volume);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Volume not found, using default: %d%%", default_volume);
        *volume = default_volume;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading volume: %s", esp_err_to_name(err));
        *volume = default_volume;
    } else {
        ESP_LOGI(TAG, "Loaded volume: %d%%", *volume);
    }
    
    // 关闭句柄
    nvs_close(handle);
    return err;
}

/**
 * 保存音量值到 extra_data 分区
 * 
 * @param volume 音量值 (0-100)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_volume_to_extra(uint8_t volume)
{
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_NVS, 
        "extra_data"
    );

    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find extra_data partition");
        return ESP_ERR_NOT_FOUND;
    }

    if (volume > 100) {
        volume = 100;
    }

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase extra_data partition: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_partition_write(partition, 0, &volume, sizeof(volume));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write volume to extra_data partition: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Volume %d%% saved to extra_data partition successfully", volume);
    }

    return err;
}

/**
 * 读取音量值从 extra_data 分区
 * 
 * @param volume 指向存储音量值的变量的指针
 * @param default_volume 如果没有存储的值，使用的默认音量
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_volume_from_extra(uint8_t *volume, uint8_t default_volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 
        ESP_PARTITION_SUBTYPE_DATA_NVS, 
        "extra_data"
    );

    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find extra_data partition");
        *volume = default_volume;
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_read(partition, 0, volume, sizeof(*volume));
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "Volume not found in extra_data, using default: %d%%", default_volume);
        *volume = default_volume;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading volume from extra_data: %s", esp_err_to_name(err));
        *volume = default_volume;
    } else {
        ESP_LOGI(TAG, "Loaded volume from extra_data: %d%%", *volume);
        if (*volume < 30 || *volume > 100) {
            ESP_LOGW(TAG, "Volume out of range (30-100), using default: %d%%", default_volume);
            *volume = default_volume;
        }
    }


    return err;
}

/**
 * 保存失电状态
 * 
 * @param power_loss_state 失电状态 (0 或 1)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_power_loss_state(uint8_t power_loss_state)
{
    nvs_handle_t handle;
    esp_err_t err;

    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 写入失电状态
    err = nvs_set_u8(handle, "power_loss", power_loss_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving power loss state: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Power loss state %d saved successfully", power_loss_state);
    }

    // 提交更改
    nvs_commit(handle);

    // 关闭句柄
    nvs_close(handle);
    return err;
}

/**
 * 读取失电状态
 * 
 * @param power_loss_state 指向存储失电状态的变量的指针
 * @param default_state 如果没有存储的值，使用的默认状态
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_power_loss_state(uint8_t *power_loss_state, uint8_t default_state)
{
    if (power_loss_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err;

    // 打开 NVS 句柄
    err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        *power_loss_state = default_state;
        return err;
    }

    // 读取失电状态
    err = nvs_get_u8(handle, "power_loss", power_loss_state);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Power loss state not found, using default: %d", default_state);
        *power_loss_state = default_state;
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error loading power loss state: %s", esp_err_to_name(err));
        *power_loss_state = default_state;
    } else {
        ESP_LOGI(TAG, "Loaded power loss state: %d", *power_loss_state);
    }

    // 关闭句柄
    nvs_close(handle);
    return err;
}

