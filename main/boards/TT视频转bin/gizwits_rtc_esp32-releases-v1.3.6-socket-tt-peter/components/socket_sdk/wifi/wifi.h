#ifndef _WIFI_H_
#define _WIFI_H_

#include "esp_err.h"

// 添加错误码
#define ESP_ERR_WIFI_CONNECTING    0x200   // WiFi正在连接中
#define WIFI_SSID_MAX_LEN         64       // WiFi SSID最大长度
#define WIFI_PASSWORD_MAX_LEN     64       // WiFi 密码最大长度

// 添加 WiFi 连接回调函数类型
typedef void (*wifi_connected_callback_t)(void);
typedef void (*wifi_disconnected_callback_t)(void);

/**
 * @brief 初始化 WiFi
 * 
 * @param connect_cb 连接成功后的回调函数
 * @param disconnect_cb 断开连接后的回调函数
 * @return esp_err_t ESP_OK: 成功, 其他: 失败
 */
esp_err_t wifi_init(wifi_connected_callback_t connect_cb, 
                    wifi_disconnected_callback_t disconnect_cb);

/**
 * @brief 连接到指定的 WiFi
 * 
 * @param ssid WiFi 名称
 * @param password WiFi 密码
 * @return esp_err_t ESP_OK: 成功, 其他: 失败
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

/**
 * @brief 连接到指定的 WiFi 并切换到 STA 模式
 * 
 * @param ssid WiFi 名称
 * @param password WiFi 密码
 */
esp_err_t wifi_connect_and_switch_to_sta(const char *ssid, const char *password);

/**
 * @brief 断开 WiFi 连接
 * 
 * @return esp_err_t ESP_OK: 成功, 其他: 失败
 */
esp_err_t wifi_disconnect(void);

/**
 * @brief 完全停止并清理WiFi资源
 * 
 * @return esp_err_t ESP_OK: 成功, 其他: 失败
 */
esp_err_t app_wifi_deinit(void);

/**
 * @brief 保存WiFi凭证到NVS
 * @param ssid WiFi名称
 * @param password WiFi密码
 * @return ESP_OK成功，其他失败
 */
esp_err_t wifi_save_config(const char *ssid, const char *password);

/**
 * @brief 从NVS加载WiFi凭证
 * @param ssid 输出缓冲区，存储SSID
 * @param ssid_size SSID缓冲区大小
 * @param password 输出缓冲区，存储密码
 * @param password_size 密码缓冲区大小
 * @return ESP_OK成功，其他失败
 */
esp_err_t wifi_load_config(char *ssid, size_t ssid_size, 
                          char *password, size_t password_size);

/**
 * @brief 清除保存的WiFi凭证
 * @return ESP_OK成功，其他失败
 */
esp_err_t wifi_clear_config(void);

extern bool wifi_is_connected;
extern bool wifi_is_connecting;    // 添加连接中的状态标志
extern char wifi_ssid[WIFI_SSID_MAX_LEN + 1];  // 当前连接的WiFi SSID
#define set_onboarding_on(value) __set_onboarding_on(__func__, __LINE__, value)
void __set_onboarding_on( const char *funN, int32_t line, uint8_t value);

bool get_esp_wifi_is_connected(void);
void clear_restart_flag(void);
#endif /* _WIFI_H_ */
