#ifndef AP_CONFIG_H
#define AP_CONFIG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi连接成功回调函数类型
 * 
 * @param ssid 连接的SSID
 * @param password 连接的密码
 * @param uid 用户ID（如果有）
 */
typedef void (*wifi_connect_success_cb_t)(const char* ssid, const char* password, const char* uid);

/**
 * @brief WiFi连接失败回调函数类型
 * 
 * @param ssid 尝试连接的SSID
 * @param password 尝试连接的密码
 * @param uid 用户ID（如果有）
 */
typedef void (*wifi_connect_fail_cb_t)(const char* ssid, const char* password, const char* uid);

/**
 * @brief AP连接回调函数类型
 * 
 * @param mac 连接的设备的MAC地址
 * @param aid 连接的设备的AID
 */
typedef void (*wifi_ap_connect_cb_t)(const uint8_t* mac, uint8_t aid);

/**
 * @brief AP断开回调函数类型
 * 
 * @param mac 断开的设备的MAC地址
 * @param aid 断开的设备的AID
 */
typedef void (*wifi_ap_disconnect_cb_t)(const uint8_t* mac, uint8_t aid);

/**
 * @brief 启动WiFi配置接入点
 * 
 * @param ssid_prefix SSID前缀（将附加MAC地址）。如果为NULL或空，将使用"ESP32"
 * @param language Web界面的语言代码。如果为NULL或空，将使用"zh-CN"
 */
void ap_config_start(const char* ssid_prefix, const char* language);

/**
 * @brief 启动WiFi配置接入点，带连接回调
 * 
 * @param ssid_prefix SSID前缀（将附加MAC地址）。如果为NULL或空，将使用"ESP32"
 * @param language Web界面的语言代码。如果为NULL或空，将使用"zh-CN"
 * @param success_cb WiFi连接成功时的回调函数
 * @param fail_cb WiFi连接失败时的回调函数
 * @param ap_connect_cb AP连接时的回调函数
 * @param ap_disconnect_cb AP断开时的回调函数
 */
void ap_config_start_with_callbacks(const char* ssid_prefix, const char* language,
                                   wifi_connect_success_cb_t success_cb,
                                   wifi_connect_fail_cb_t fail_cb,
                                   wifi_ap_connect_cb_t ap_connect_cb,
                                   wifi_ap_disconnect_cb_t ap_disconnect_cb);

/**
 * @brief 停止WiFi配置接入点
 */
void ap_config_stop();

/**
 * @brief 启动SmartConfig进行WiFi配置
 */
void ap_config_start_smartconfig();

/**
 * @brief 获取配置Web服务器的URL
 * 
 * @return const char* Web服务器的URL（通常为"http://10.10.100.254"）
 */
const char* ap_config_get_url();

#ifdef __cplusplus
}
#endif

#endif // AP_CONFIG_H 