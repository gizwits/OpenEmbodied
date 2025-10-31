#ifndef _BUTTON_H_
#define _BUTTON_H_

#include <stdbool.h>
#include "esp_err.h"

// 按钮事件类型
typedef enum {
    BUTTON_EVENT_PRESSED = 0,     // 按下事件
    BUTTON_EVENT_RELEASED,        // 释放事件
    BUTTON_EVENT_CLICKED,         // 单击事件(按下后快速释放)
    BUTTON_EVENT_LONG_PRESSED,    // 长按事件
    BUTTON_EVENT_DOUBLE_CLICKED, // 双击事件
    BUTTON_EVENT_TRIPLE_CLICKED   // 三击事件
} button_event_t;

// 按钮配置
typedef struct {
    uint32_t long_press_time;     // 长按时间阈值(ms)
    uint32_t double_click_time;   // 双击间隔时间(ms)
} button_config_t;

// 默认配置
#define BUTTON_DEFAULT_CONFIG() { \
    .long_press_time = 2000, \
    .double_click_time = 300 \
}

static const button_config_t button_default_config = {
    .long_press_time = 2000,
    .double_click_time = 300
};

// 按钮事件回调函数类型
typedef void (*button_callback_t)(button_event_t event);

/**
 * @brief 初始化按钮
 * 
 * @param gpio_num GPIO引脚号
 * @param config 按钮配置，传 NULL 则使用默认配置
 * @param callback 事件回调函数
 * @return esp_err_t 
 */
esp_err_t board_button_init(int gpio_num, const button_config_t *config, button_callback_t callback);

#endif // _BUTTON_H_ 