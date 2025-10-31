#ifndef LED_PWM_H
#define LED_PWM_H

#include "esp_err.h"

// 宏定义
#define LED_PWM_TIMER_COLD     LEDC_TIMER_0  // 冷色温定时器
#define LED_PWM_TIMER_WARM     LEDC_TIMER_1  // 暖色温定时器
#define LED_PWM_MODE           LEDC_LOW_SPEED_MODE
#define LED_PWM_CH_COLD        LEDC_CHANNEL_0 // 冷色温通道
#define LED_PWM_CH_WARM        LEDC_CHANNEL_1 // 暖色温通道
#define LED_PWM_GPIO_COLD      11 // 冷色温LED引脚,根据实际硬件修改
#define LED_PWM_GPIO_WARM      13 // 暖色温LED引脚,根据实际硬件修改
#define LED_PWM_DUTY_RES       LEDC_TIMER_13_BIT // 13位分辨率
#define LED_PWM_FREQ           5000  // PWM频率5KHz
#define LED_PWM_INCREMENT_INTERVAL 2000 // 每2秒增加一次亮度
#define LED_PWM_MAX_DUTY       8191  // 13位分辨率下的最大占空比值 2的13次方-1

// API声明
esp_err_t led_pwm_init(void);
esp_err_t led_pwm_set_duty_cold(uint8_t duty_percent);
esp_err_t led_pwm_set_duty_warm(uint8_t duty_percent);
esp_err_t led_pwm_set_duty_both(uint8_t duty_percent_cold, uint8_t duty_percent_warm);
void led_state_step_up(void);
void led_state_step_down(void); 
void led_fade_cycle(void);
#endif // LED_PWM_H
