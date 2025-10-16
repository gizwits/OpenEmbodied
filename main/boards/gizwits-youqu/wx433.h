#ifndef __WX433_H__
#define __WX433_H__

// INSERT OTHER CONTENT HERE IF NEEDED

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "string.h"

typedef enum {
    WTP_PIN_LEG = 1,
    WTP_PIN_CHEST = 2,
} wtp_pin_t;

typedef enum {
    MUTE = 0,           // 静音
    CLIP_MINUS,         // 夹吸-
    CLIP_PLUS,          // 夹吸+
    MODE,               // 模式
    VOLUME_MINUS,       // 音量-
    VOLUME_PLUS,        // 音量+
    VIBRATION_MINUS,    // 震动-
    VIBRATION_PLUS,     // 震动+
    TX_MAX
} WX433_TX_EVENT_t;

// 定义433事件类型
typedef enum {
    WX433_EVENT_NONE = 0,
    WX433_EVENT_LEG_TOUCH = 1,    // 腿部触摸事件
    WX433_EVENT_CHEST_TOUCH = 2,  // 胸部触摸事件
    WX433_EVENT_TX = 3,           // 发送事件
} wx433_event_type_t;

// 定义433事件结构体
typedef struct {
    wx433_event_type_t type;      // 事件类型
    union {
        struct {
            const char *message;   // 动作消息
            int touch_pin;         // 触摸引脚
        } touch;
        struct {
            WX433_TX_EVENT_t event; // 发送的事件类型
        } tx;
    } data;
} wx433_event_t;

// 定义事件回调函数类型
typedef void (*wx433_event_callback_t)(const wx433_event_t *event, void *user_data);

/**
 * @brief Initializes the WX433 module.
 */
void wx433_init(void);

/**
 * @brief Retrieves weather data.
 * 
 * @param output_buffer Pointer to the buffer where the weather data will be stored.
 * @param output_length Pointer to the size of the output buffer.
 * @param isPrintfLog Flag indicating whether to print the log.
 * @return Returns an integer indicating the success or failure of the operation.
 */
int32_t get_weather_data(uint8_t *output_buffer, size_t *output_length, bool isPrintfLog);

/**
 * @brief Sets a weather event.
 * 
 * @param event The weather event to be set.
 * @return Returns an integer indicating the success or failure of the operation.
 */
int32_t set_weather_event(WX433_TX_EVENT_t event);

/**
 * @brief 注册433事件监听器
 * 
 * @param callback 事件回调函数
 * @param user_data 用户数据指针
 * @return ESP_OK: 成功, 其他: 失败
 */
esp_err_t wx433_register_event_listener(wx433_event_callback_t callback, void *user_data);

/**
 * @brief 注销433事件监听器
 * 
 * @param callback 要注销的回调函数
 * @return ESP_OK: 成功, 其他: 失败
 */
esp_err_t wx433_unregister_event_listener(wx433_event_callback_t callback);

#endif /* __WX433_H__ */

