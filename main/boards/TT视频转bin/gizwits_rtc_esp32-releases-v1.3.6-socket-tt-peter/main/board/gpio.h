#ifndef _LED_H_
#define _LED_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

// 在枚举前添加优先级定义
typedef enum {
    LED_PRIORITY_LOW = 0,    // 可被任何状态打断
    LED_PRIORITY_NORMAL = 1, // 普通优先级
    LED_PRIORITY_HIGH = 2,   // 高优先级，不可打断
} led_priority_t;

// LED 状态枚举
typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_INIT = 1,
    LED_STATE_WIFI_CONNECTED = 2,
    LED_STATE_WIFI_CONNECTING = 3,
    LED_STATE_RESET = 4,
    LED_STATE_CHARGING = 5,
    LED_STATE_FULL_BATTERY = 6,
    LED_STATE_SLEEPING = 7,    // 休眠
    LED_STATE_USER_SPEAKING = 8,     // 用户说话（RGB幻彩闪烁）
    LED_STATE_AI_SPEAKING = 9,
    LED_STATE_POOR_NETWORK = 10,      // 网络不佳（橙色慢闪）
    LED_EFFECT_SOLID = 11,
    LED_STATE_STANDBY = 12,
    LED_STATE_BLUE_BREATH = 13,  // 添加蓝色呼吸状态
    LED_STATE_NO_CONNECT_WIFI = 14,  // 未连接Wi-Fi
    LED_STATE_TEST_LED = 15,     // 测试LED状态
    LED_STATE_AGING = 16,        // 老化测试
    LED_STATE_MAX
} led_state_t;

// 修改 LED 状态枚举，添加优先级信息
typedef struct {
    led_state_t state;
    led_priority_t priority;
} led_control_t;

// LED 呼吸灯配置结构体
typedef struct {
    uint8_t r;          // 红色亮度 0-255 
    uint8_t g;          // 绿色亮度
    uint8_t b;          // 蓝色亮度
    uint32_t period_ms; // 一次呼吸周期(毫秒)
    int32_t times;      // 呼吸次数,-1表示持续呼吸
    float min_bright;   // 最小亮度 0.0-1.0
    float max_bright;   // 最大亮度 0.0-1.0
} breath_config_t;

// LED 跑马灯配置结构体
typedef struct {
    uint8_t r;          // 红色亮度 0-255
    uint8_t g;          // 绿色亮度
    uint8_t b;          // 蓝色亮度
    uint8_t start_pos;  // 起始位置 0-3
    uint32_t rounds;     // 运行圈数
    uint32_t delay_ms;  // 每步延时(ms)
} marquee_config_t;

// LED效果类型
typedef enum {
    LED_EFFECT_NONE = 0,
    LED_EFFECT_BREATH,
    LED_EFFECT_MARQUEE,
    LED_EFFECT_STOP,
    LED_EFFECT_TEST_LED
} led_effect_t;

// LED效果控制消息
typedef struct {
    led_effect_t effect;     // 效果类型
    union {
        breath_config_t breath;   // 呼吸灯配置
        marquee_config_t marquee; // 跑马灯配置
        struct {                  // 流水灯配置
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint32_t period_ms;
            uint32_t delay_ms;    // 每步延时(ms)
        } flow;
        struct {                  // 流水灯配置
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } color;
    } config;
} led_effect_msg_t;

// 按键到LED位置的映射
typedef enum {
    LED_POS_PLAY = 2, 
    LED_POS_VOLUP = 1,
    LED_POS_VOLDOWN = 3,
    LED_POS_REC = 0
} led_position_t;

// 按键ID到LED位置的映射函数声明
uint8_t key_to_led_position(int key_id);

// LED颜色结构体
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// 获取按键对应的LED颜色
led_color_t get_key_led_color(int key_id);

void gpio_init(void);
void led_set_state(led_state_t state);
void led_task_stop(void);  // 停止当前灯效任务
void gpio_set_power_status(int status);
void init_power_hold_gpio(int status);

void led_strip_init(void);
void led_strip_update(uint8_t state);
void led_strip_breath(breath_config_t config);
void led_strip_marquee(marquee_config_t config);
void led_strip_test_led(uint8_t r, uint8_t g, uint8_t b);

// LED效果控制函数
void led_effect_start(led_effect_msg_t *msg);
void led_effect_stop(void);
void led_set_rgb(int r, int g, int b);
/**
 * @brief Set the global LED brightness
 * 
 * @param brightness Brightness value (0-100)
 * @return ESP_OK if successful, ESP_ERR_INVALID_ARG if brightness is out of range
 */
esp_err_t led_set_global_brightness(uint8_t brightness);

/**
 * @brief Get the current global LED brightness
 * 
 * @return Current brightness value (0-100)
 */
uint8_t led_get_global_brightness(void);

#endif
