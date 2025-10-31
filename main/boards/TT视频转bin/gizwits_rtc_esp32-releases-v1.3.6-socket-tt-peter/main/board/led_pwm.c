#include "led_pwm.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"


static const char *TAG = "LED_PWM";

static esp_timer_handle_t led_brightness_timer_handle = NULL;
static uint8_t current_brightness = 0;

// 增加亮度的函数
static void increase_brightness(void *arg) {
    current_brightness += 5; // 每次增加5%
    if (current_brightness > 100) {
        current_brightness = 0; // 如果超过100%，重置为0
    }
    led_pwm_set_duty_cold(current_brightness);
    led_pwm_set_duty_warm(current_brightness);
    printf("Increasing brightness to %d%%\n", current_brightness);
}

esp_err_t led_pwm_init(void)
{
    // 在初始化函数中添加日志
    ESP_LOGI(TAG, "Initializing LED PWM...");
    printf("%s %d\n", __FILE__, __LINE__);

    // 冷色温定时器配置
    ledc_timer_config_t ledc_timer_cold = {
        .speed_mode       = LED_PWM_MODE,
        .timer_num        = LED_PWM_TIMER_COLD,
        .duty_resolution  = LED_PWM_DUTY_RES,
        .freq_hz         = LED_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_cold));
    printf("%s %d\n", __FILE__, __LINE__);

    // 暖色温定时器配置
    ledc_timer_config_t ledc_timer_warm = {
        .speed_mode       = LED_PWM_MODE,
        .timer_num        = LED_PWM_TIMER_WARM,
        .duty_resolution  = LED_PWM_DUTY_RES,
        .freq_hz         = LED_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_warm));
    printf("%s %d\n", __FILE__, __LINE__);

    // 冷色温通道配置
    ledc_channel_config_t ledc_channel_cold = {
        .speed_mode     = LED_PWM_MODE,
        .channel        = LED_PWM_CH_COLD,
        .timer_sel      = LED_PWM_TIMER_COLD,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LED_PWM_GPIO_COLD,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_cold));
    printf("%s %d\n", __FILE__, __LINE__);

    // 暖色温通道配置
    ledc_channel_config_t ledc_channel_warm = {
        .speed_mode     = LED_PWM_MODE,
        .channel        = LED_PWM_CH_WARM,
        .timer_sel      = LED_PWM_TIMER_WARM,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LED_PWM_GPIO_WARM,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel_warm));
    
#ifdef AUTO_LED_BRIGHTNESS
    // 在led_pwm_init中初始化esp_timer
    if (led_brightness_timer_handle == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = increase_brightness,
            .name = "led_brightness_timer"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &led_brightness_timer_handle));
        ESP_ERROR_CHECK(esp_timer_start_periodic(led_brightness_timer_handle, LED_PWM_INCREMENT_INTERVAL * 1000));
        ESP_LOGI(TAG, "Creating and starting LED brightness esp_timer...");
    }
#endif
    printf("%s %d\n", __FILE__, __LINE__);
    
    return ESP_OK;
}

esp_err_t led_pwm_set_duty_cold(uint8_t duty_percent)
{
    // 限制占空比在0-100%之间
    if(duty_percent > 100) {
        duty_percent = 100;
    }
    
    // 将百分比转换为实际占空比值
    uint32_t duty = (LED_PWM_MAX_DUTY * duty_percent) / 100; // 使用宏定义代替8191
    
    ESP_LOGI(TAG, "Setting cold LED duty to %d%%", duty_percent);
    
    if (ledc_get_duty(LED_PWM_MODE, LED_PWM_CH_COLD) != duty) {
        ESP_ERROR_CHECK(ledc_set_duty(LED_PWM_MODE, LED_PWM_CH_COLD, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LED_PWM_MODE, LED_PWM_CH_COLD));
    }
    
    return ESP_OK;
}

esp_err_t led_pwm_set_duty_warm(uint8_t duty_percent)
{
    // 限制占空比在0-100%之间
    if(duty_percent > 100) {
        duty_percent = 100;
    }
    
    // 将百分比转换为实际占空比值
    uint32_t duty_warm = (LED_PWM_MAX_DUTY * duty_percent) / 100; // 使用宏定义代替8191
    
    ESP_LOGI(TAG, "Setting warm LED duty to %d%%", duty_percent);
    
    if (ledc_get_duty(LED_PWM_MODE, LED_PWM_CH_WARM) != duty_warm) {
        ESP_ERROR_CHECK(ledc_set_duty(LED_PWM_MODE, LED_PWM_CH_WARM, duty_warm));
        ESP_ERROR_CHECK(ledc_update_duty(LED_PWM_MODE, LED_PWM_CH_WARM));
    }
    
    return ESP_OK;
}

// 新增函数，用于同时设置冷色温和暖色温的占空比
esp_err_t led_pwm_set_duty_both(uint8_t duty_percent_cold, uint8_t duty_percent_warm) {
    // 设置冷色温占空比
    if(duty_percent_cold > 100) {
        duty_percent_cold = 100;
    }
    uint32_t duty_cold = (LED_PWM_MAX_DUTY * duty_percent_cold) / 100;
    if (ledc_get_duty(LED_PWM_MODE, LED_PWM_CH_COLD) != duty_cold) {
        ESP_ERROR_CHECK(ledc_set_duty(LED_PWM_MODE, LED_PWM_CH_COLD, duty_cold));
        ESP_ERROR_CHECK(ledc_update_duty(LED_PWM_MODE, LED_PWM_CH_COLD));
    }

    // 设置暖色温占空比
    if(duty_percent_warm > 100) {
        duty_percent_warm = 100;
    }
    uint32_t duty_warm = (LED_PWM_MAX_DUTY * duty_percent_warm) / 100;
    if (ledc_get_duty(LED_PWM_MODE, LED_PWM_CH_WARM) != duty_warm) {
        ESP_ERROR_CHECK(ledc_set_duty(LED_PWM_MODE, LED_PWM_CH_WARM, duty_warm));
        ESP_ERROR_CHECK(ledc_update_duty(LED_PWM_MODE, LED_PWM_CH_WARM));
    }

    ESP_LOGI(TAG, "Setting cold LED duty to %d%% and warm LED duty to %d%%", duty_percent_cold, duty_percent_warm);

    return ESP_OK;
}

// 定义状态机枚举类型
typedef enum {
    LED_STATE_OFF = 0,      // 无光
    LED_STATE_COLD_FULL,    // 冷光100%
    LED_STATE_WARM_FULL,    // 暖光100%
    LED_STATE_BOTH_HALF,    // 冷暖各50%
    LED_STATE_MAX           // 状态机最大值标记
} led_state_t;

// 当前状态机状态
static led_state_t current_state = LED_STATE_OFF;

// 定义LED渐变方向
typedef enum {
    LED_FADE_UP,
    LED_FADE_DOWN
} led_fade_direction_t;

// 定义LED渐变状态
typedef struct {
    bool is_dual;           // 是否双光模式
    bool is_cold;          // 是否冷光
    uint8_t duty;          // 当前占空比
    led_fade_direction_t direction;  // 渐变方向
} led_fade_state_t;

static led_fade_state_t fade_state = {
    .is_dual = false,
    .is_cold = true,  // 默认为冷光
    .duty = 5,
    .direction = LED_FADE_UP
};


// 设置LED状态对应的占空比
static void led_state_set_duty(led_state_t state) {
    switch(state) {
        case LED_STATE_OFF:
            fade_state.is_dual = false;
            fade_state.duty = 0;
            fade_state.is_cold = true;
            ESP_LOGI(TAG, "LED state: OFF");
            led_pwm_set_duty_both(0, 0);
            break;
            
        case LED_STATE_COLD_FULL:
            fade_state.is_dual = false;
            fade_state.duty = 100;
            fade_state.is_cold = true;
            ESP_LOGI(TAG, "LED state: COLD 100%%");
            led_pwm_set_duty_both(100, 0);
            break;
            
        case LED_STATE_WARM_FULL:
            fade_state.is_dual = false;
            fade_state.duty = 100;
            fade_state.is_cold = false;
            ESP_LOGI(TAG, "LED state: WARM 100%%");
            led_pwm_set_duty_both(0, 100);
            break;
            
        case LED_STATE_BOTH_HALF:
            fade_state.is_dual = true;
            fade_state.duty = 50;
            fade_state.is_cold = true;
            ESP_LOGI(TAG, "LED state: BOTH 50%%");
            led_pwm_set_duty_both(50, 50);
            break;
            
        default:
            ESP_LOGE(TAG, "Invalid LED state!");
            break;
    }
}

// 定义LED无极调光步长
#define FADE_STEP 3
// LED无极调光函数
void led_fade_cycle(void) {
    // 根据当前方向调整占空比
    if(fade_state.direction == LED_FADE_UP) {
        // 递增模式
        if((!fade_state.is_dual && fade_state.duty >= 100) || 
           (fade_state.is_dual && fade_state.duty >= 50)) {
            // 达到最大值,切换方向
            fade_state.direction = LED_FADE_DOWN;
            fade_state.duty -= FADE_STEP;
        } else {
            fade_state.duty += FADE_STEP;
        }
    } else {
        // 递减模式
        if(fade_state.duty <= 5) {
            // 达到最小值,切换方向和模式
            fade_state.direction = LED_FADE_UP;
            fade_state.is_dual = !fade_state.is_dual;
            fade_state.duty += FADE_STEP;
        } else {
            fade_state.duty -= FADE_STEP;
        }
    }

    // 设置LED占空比
    if(fade_state.is_dual) {
        ESP_LOGI(TAG, "LED state: BOTH  %d%%", fade_state.duty);
        led_pwm_set_duty_both(fade_state.duty, fade_state.duty);
    } else {
        ESP_LOGI(TAG, "LED state: SINGLE %d%%", fade_state.duty);
        if(fade_state.is_cold) {
            led_pwm_set_duty_cold(fade_state.duty);
            led_pwm_set_duty_warm(0);
        } else {
            led_pwm_set_duty_cold(0);
            led_pwm_set_duty_warm(fade_state.duty);
        }
    }
}

// 状态机步进函数 - 增加状态
void led_state_step_up(void) {
    current_state = (current_state + 1) % LED_STATE_MAX;
    led_state_set_duty(current_state);
}

// 状态机步进函数 - 减少状态 
void led_state_step_down(void) {
    if(current_state == LED_STATE_OFF) {
        current_state = LED_STATE_MAX - 1;
    } else {
        current_state--;
    }
    led_state_set_duty(current_state);
}
