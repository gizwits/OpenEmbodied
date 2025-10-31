#ifndef BLE_H
#define BLE_H

#include <stdbool.h>
#include "esp_err.h"


// BLE 初始化
void ble_init(bool onboarding_flag);

// BLE 停止
void ble_stop(void);

/**
 * @brief 设置广播包中的设备名称和自定义数据
 * 
 * @param device_name 设备名称
 * @param pk 4字节的 PK 短编码
 * @param mac 6字节的 MAC 地址
 * @return esp_err_t ESP_OK: 成功, 其他: 失败
 */
esp_err_t ble_set_adv_data(const char *device_name, uint32_t pk, const uint8_t *mac);

/**
 * @brief 设置设备的配网状态
 * 
 * @param configured true: 已配网, false: 未配网
 */
void ble_set_network_status(bool configured);

/**
 * @brief 通过BLE发送通知数据
 * @param data 要发送的数据
 * @param len 数据长度
 * @return true成功，false失败
 */
bool ble_send_notify(const uint8_t *data, size_t len);

#endif // BLE_H
