#ifndef __LEDC_H__
#define __LEDC_H__

#include "led_strip.h"

// LED 业务枚举
typedef enum {
    TT_LED_STATE_OFF = 0,      // 倾听 思考 回复 打断播放内容 关机
    TT_LED_STATE_PURPLE = 1,   // 请改成业务名称
    TT_LED_STATE_RED = 2,      // 电量低于20%
    TT_LED_STATE_ORANGE = 3,   // 关闭盖子或30s无交流，长按2~3秒开机键开机
    TT_LED_STATE_WHITE = 4,    // 唤醒 配网/联网成功
    TT_LED_STATE_GREEN = 5,    // 绿灯状态
    TT_LED_STATE_BLUE = 6,     // 蓝灯状态
    TT_LED_STATE_YELLOW = 7,   // 黄灯状态
    TT_LED_STATE_MAX
} tt_led_state_t;    // 动这个连带动 static const char* state_names[]


// LED效果类型
typedef enum {
    TT_LED_EFFECT_STOP = 0,    // 关闭灯光
    TT_LED_EFFECT_BREATH,      // 呼吸灯光
    TT_LED_EFFECT_MARQUEE,     // 跑马灯光    
    TT_LED_EFFECT_FLOW,        // 流水灯光 
    TT_LED_EFFECT_NONE,        // 常亮灯光
    TT_LED_EFFECT_MAX, 
} tt_led_effect_t;

// LED 呼吸灯配置结构体
typedef struct {
    uint8_t hr;          // 红色亮度 高亮度 
    uint8_t hg;          // 绿色亮度
    uint8_t hb;          // 蓝色亮度
    uint8_t lr;          // 红色亮度 低亮度 
    uint8_t lg;          // 绿色亮度
    uint8_t lb;          // 蓝色亮度
    uint32_t period_ms; // 一次呼吸周期(毫秒)
    int32_t times;      // 呼吸次数,-1表示持续呼吸
    float min_bright;   // 最小亮度 0.0-1.0
    float max_bright;   // 最大亮度 0.0-1.0
} tt_breath_config_t;

// LED 跑马灯配置结构体
typedef struct {
    uint8_t r;          // 红色亮度 0-255
    uint8_t g;          // 绿色亮度
    uint8_t b;          // 蓝色亮度
    uint8_t start_pos;  // 起始位置 0-32
    uint8_t rounds;     // 运行圈数
    uint32_t delay_ms;  // 每步延时(ms)
} tt_marquee_config_t;

// 流水灯配置
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t period_ms; // 一次跑马周期(毫秒)
    uint32_t delay_ms; // 每步延时(ms)
} flow_config_t;

// 常亮灯配置
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} none_config_t;

typedef struct {
    tt_led_effect_t effect;     // 效果类型
    union {
        tt_breath_config_t     breath;  // 呼吸灯配置
        tt_marquee_config_t    marquee; // 跑马灯配置
        flow_config_t       flow;    // 流水灯配置
        none_config_t       none;    // 常亮灯配置
    } config;
} tt_led_effect_msg_t;

void tt_led_strip_init(void);
tt_led_state_t get_tt_led_last_state(void);
#define tt_led_strip_set_state(state) __tt_led_strip_set_state(__func__, __LINE__, state)
void __tt_led_strip_set_state(const char * func, uint32_t line, tt_led_state_t state);

#endif /* __LEDC_H__ */
