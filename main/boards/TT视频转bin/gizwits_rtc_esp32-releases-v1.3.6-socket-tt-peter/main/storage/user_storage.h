#ifndef USER_STORAGE_H
#define USER_STORAGE_H

#include "esp_err.h"
#include <stdint.h>

/**
 * 初始化存储系统
 * 
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_init(void);

/**
 * 保存 RGB 颜色值
 * 
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_rgb_color(uint8_t r, uint8_t g, uint8_t b);

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
                                     uint8_t default_r, uint8_t default_g, uint8_t default_b);

/**
 * 保存 RGB 亮度值
 * 
 * @param brightness 亮度值 (0-100)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_brightness(uint8_t brightness);

/**
 * 读取 RGB 亮度值
 * 
 * @param brightness 指向存储亮度值的变量的指针
 * @param default_brightness 如果没有存储的值，使用的默认亮度
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_brightness(uint8_t *brightness, uint8_t default_brightness);

/**
 * 保存音量值
 * 
 * @param volume 音量值 (0-100)
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_save_volume(uint8_t volume);

/**
 * 读取音量值
 * 
 * @param volume 指向存储音量值的变量的指针
 * @param default_volume 如果没有存储的值，使用的默认音量
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t user_storage_load_volume(uint8_t *volume, uint8_t default_volume);

#endif /* USER_STORAGE_H */ 