#ifndef TOUCH_KEY_H
#define TOUCH_KEY_H

#include "driver/touch_pad.h"

// 触摸按键配置
#define TOUCH_PAD_NO_CHANGE   (-1)
#define TOUCH_THRESH_NO_USE   (0)
#define TOUCH_FILTER_MODE     (0)
#define TOUCHPAD_FILTER_TOUCH_PERIOD (10)

// 触摸按键状态
typedef enum {
    TOUCH_IDLE = 0,     // 空闲状态
    TOUCH_PRESS,        // 按下状态
    TOUCH_LONG_PRESS,   // 长按状态
} touch_pad_state_t;

// 触摸事件类型
typedef enum {
    TOUCH_EVT_PRESS = 0,       // 按下事件
    TOUCH_EVT_RELEASE,         // 释放事件
    TOUCH_EVT_LONG_PRESS,      // 长按事件
} touch_pad_evt_t;

// 触摸按键配置结构体
typedef struct {
    touch_pad_t pad_num;       // 触摸按键编号
    uint32_t thresh;           // 触发阈值
    uint32_t long_press_time;  // 长按时间阈值(ms)
} touch_pad_config_t;

// 触摸按键状态结构体
typedef struct {
    touch_pad_state_t state;   // 当前状态
    uint32_t press_start;      // 按下开始时间
    uint32_t last_press;       // 上次按下时间
} touch_pad_state_info_t;

// 回调函数定义
typedef void (*touch_callback_t)(touch_pad_evt_t evt);

// API 函数声明
void touch_pad_register_callback(touch_callback_t cb);
void touch_pad_start(void);

#endif // TOUCH_KEY_H

