#include "board/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "led_strip.h"
#include "board.h"
#include "input_key_service.h"

static const char *TAG = "GPIO";



#if (defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2))
#define BLINK_GPIO              GPIO_NUM_2
#define MAX_LED_NUM 4
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#define BLINK_GPIO              GPIO_NUM_2
#define MAX_LED_NUM 28
#elif defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
#define BLINK_GPIO              GPIO_NUM_7
#define MAX_LED_NUM 4
#endif

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#define EXT_POWER_EN_GPIO      GPIO_NUM_13
#endif

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
void init_ext_power_en_gpio(int status) {
    ESP_LOGI(TAG, "init_ext_power_en_gpio: %d, %d", EXT_POWER_EN_GPIO, status);
    gpio_drive_init(GPIO_MODE_OUTPUT, 0, 0, GPIO_INTR_DISABLE, EXT_POWER_EN_GPIO, status);
}

void gpio_set_ext_power_en(uint8_t status) {
    ESP_LOGI(TAG, "gpio_set_ext_power_en: %d, %d", EXT_POWER_EN_GPIO, status);
    gpio_set_status(EXT_POWER_EN_GPIO, status);
}
#else
void init_ext_power_en_gpio(int status) {}
void gpio_set_ext_power_en(uint8_t status) {}
#endif

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1) || defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
static void pwm_init(void);
static void set_rgb(uint8_t r, uint8_t g, uint8_t b);
static TaskHandle_t led_task_handle = NULL;
static led_state_t current_state = LED_STATE_OFF;
static QueueHandle_t led_queue = NULL;
static TaskHandle_t led_manager_task_handle = NULL;
static void led_control_task(void *arg);

// PWM配置参数
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES          LEDC_TIMER_8_BIT  // 8位分辨率，值范围0-255
#define LEDC_FREQUENCY         5000              // PWM频率5KHz

// 为RGB LED配置LEDC通道
#define LEDC_CHANNEL_RED       LEDC_CHANNEL_0
#define LEDC_CHANNEL_GREEN     LEDC_CHANNEL_1
#define LEDC_CHANNEL_BLUE      LEDC_CHANNEL_2

static int rgb_r_brightness = 255;
static int rgb_g_brightness = 255;
static int rgb_b_brightness = 255;

void led_set_rgb(int r, int g, int b) {
    rgb_r_brightness = r;
    rgb_g_brightness = g;
    rgb_b_brightness = b;

#if defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    esp_err_t led_pwm_set_duty_both(uint8_t duty_percent_cold, uint8_t duty_percent_warm);
    led_pwm_set_duty_both(r, g);
#endif
}

// Add at the top with other global variables
static uint8_t global_led_brightness = 60; // Default to 100% brightness (0-100 scale)

/**
 * @brief Set the global LED brightness
 * 
 * @param brightness Brightness value (0-100)
 * @return ESP_OK if successful, ESP_ERR_INVALID_ARG if brightness is out of range
 */
esp_err_t led_set_global_brightness(uint8_t brightness) {
    if (brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    global_led_brightness = brightness;
    ESP_LOGI(TAG, "Global LED brightness set to %d%%", brightness);

#if defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    esp_err_t led_pwm_set_duty_both(uint8_t duty_percent_cold, uint8_t duty_percent_warm);
    led_pwm_set_duty_both(brightness, brightness);
#endif

    return ESP_OK;
}

/**
 * @brief Get the current global LED brightness
 * 
 * @return Current brightness value (0-100)
 */
uint8_t led_get_global_brightness(void) {
    return global_led_brightness;
}

/**
 * @brief Apply global brightness to a color value
 * 
 * @param color_value Original color value (0-255)
 * @return Adjusted color value based on global brightness
 */
static uint8_t apply_brightness(uint8_t color_value) {
    return (color_value * global_led_brightness) / 100;
}

void gpio_set_status(uint32_t pinNumber, uint8_t status)
{
    gpio_set_level(pinNumber, status);
}

void gpio_drive_init(gpio_mode_t gpioMode, gpio_pullup_t pullUpEnable, gpio_pulldown_t pullDownEnable, gpio_int_type_t interruptType, uint32_t pinNumber, uint32_t initialLevel)
{
    gpio_config_t gpioConfig = {
        .intr_type = interruptType,
        .mode = gpioMode,
        .pin_bit_mask = (1ULL << pinNumber),
        .pull_down_en = pullDownEnable,
        .pull_up_en = pullUpEnable
    };
    gpio_set_level(pinNumber, initialLevel); // Set the power amplifier to the specified state during initialization
    gpio_config(&gpioConfig);
    gpio_set_level(pinNumber, initialLevel); // Set the power amplifier to the specified state during initialization
}

// 初始化GPIO
void gpio_init(void)
{
    ESP_LOGI(TAG, "gpio_init");
    
//    init_power_hold_gpio(1);

    // 初始化其他GPIO
    // gpio_drive_init(GPIO_MODE_OUTPUT, 0, 0, GPIO_INTR_DISABLE, get_power_hold_gpio(), 1);

    // 初始化PWM
#ifndef CONFIG_AUDIO_BOARD_TT_MUSIC_V1
    pwm_init();
#endif

    init_pa();

    
    // 设置初始LED状态
    // set_rgb(1, 0, 1);  // 默认绿灯亮
}

void init_power_hold_gpio(int status) {
    gpio_drive_init(GPIO_MODE_OUTPUT, 1, 0, GPIO_INTR_DISABLE, get_power_hold_gpio(), status);
}


void gpio_set_blue_led(uint8_t status)
{
    // printf("gpio set blue led: %d to %d\n", get_blue_led_gpio(), status);
    if (get_blue_led_gpio() >= 0) {
        gpio_set_status(get_blue_led_gpio(), status);
    }
}

void gpio_set_red_led(uint8_t status)
{
    // printf("gpio set red led: %d to %d\n", get_red_led_gpio(), status);
    if (get_red_led_gpio() >= 0) {
        gpio_set_status(get_red_led_gpio(), status);
    }
}

void gpio_set_green_led(uint8_t status)
{
    // printf("gpio set green led: %d to %d\n", get_green_led_gpio(), status);
    if (get_green_led_gpio() >= 0) {
        gpio_set_status(get_green_led_gpio(), status);
    }
}

// LED控制基础函数
static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    // Apply global brightness to each color component
    uint8_t adjusted_r = r ? apply_brightness(255) : 0;
    uint8_t adjusted_g = g ? apply_brightness(255) : 0;
    uint8_t adjusted_b = b ? apply_brightness(255) : 0;
    
    // Note: If LEDs are common anode, you might need to invert the values
    if (get_red_led_gpio() >= 0) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, adjusted_r);
    }
    if (get_green_led_gpio() >= 0) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, adjusted_g);
    }
    if (get_blue_led_gpio() >= 0) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, adjusted_b);
    }
    
    if (get_red_led_gpio() >= 0) {
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);
    }
    if (get_green_led_gpio() >= 0) {
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN);
    }
    if (get_blue_led_gpio() >= 0) {
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE);
    }
}

// 初始化 LED 管理
static void led_manager_init(void) {
    if (led_queue == NULL) {
        led_queue = xQueueCreate(5, sizeof(led_state_t));
    }
}

// LED 管理任务
static void led_manager_task(void *arg) {
    led_state_t new_state;
    
    while (1) {
        if (xQueueReceive(led_queue, &new_state, portMAX_DELAY) == pdTRUE) {
            if (current_state == new_state) {
                continue;
            }
            
            // 停止当前运行的LED任务
            if (led_task_handle != NULL) {
                vTaskDelete(led_task_handle);
                led_task_handle = NULL;
                set_rgb(1, 1, 1); 
            }
            
            if (new_state == LED_STATE_OFF) {
                current_state = LED_STATE_OFF;
                continue;
            }

            // 创建新的LED控制任务
            current_state = new_state;
            xTaskCreatePinnedToCore(led_control_task, 
                       "led_task", 
                       1024 * 2,
                       (void*)new_state, 
                       3,
                       &led_task_handle,
                       0);
        }
    }
}

// LED控制任务
static void led_control_task(void *arg) {
    led_state_t state = (led_state_t)arg;
    uint8_t count = 0;
    uint8_t brightness = 0;
    bool increasing = true;

    while (1) {
        switch (state) {
            case LED_STATE_INIT: {
                set_rgb(1, 1, 0); 
                vTaskDelay(pdMS_TO_TICKS(1000));  // 延时1秒
                set_rgb(1, 1, 1);  // 全部关闭
                vTaskDelay(pdMS_TO_TICKS(1000));  // 延时1秒
                break;
            }

            case LED_STATE_BLUE_BREATH: {
                // 蓝色呼吸效果
                if (increasing) {
                    brightness += 5;
                    if (brightness >= 255) {
                        increasing = false;
                    }
                } else {
                    brightness -= 5;
                    if (brightness <= 0) {
                        increasing = true;
                    }
                }
                
                // 设置蓝色LED亮度，其他颜色关闭
                if (get_red_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, 0);
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, 0);
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, brightness);
                }
                
                if (get_red_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN);
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE);
                }
                
                vTaskDelay(pdMS_TO_TICKS(20));  // 控制呼吸速度
                break;
            }
                    
            case LED_STATE_SLEEPING: {
                set_rgb(1, 1, 1);  // 休眠
                break;
            }
            
            case LED_STATE_WIFI_CONNECTED: {
                if (count >= 6) {
                    led_task_stop();
                    return;
                }
                int v = count & 1;
                set_rgb(v, v, v);
                vTaskDelay(pdMS_TO_TICKS(200));
                count++;
                break;
            }

            case LED_STATE_RESET: {
                if (count >= 6) {
                    led_task_stop();
                    return;
                }
                set_rgb(1, 1, count & 1);
                vTaskDelay(pdMS_TO_TICKS(200));
                count++;
                break;
            }

            case LED_STATE_AGING: {
                // 红、绿、蓝、黄来回切换，间隔1秒
                set_rgb(1, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                set_rgb(0, 1, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                set_rgb(0, 0, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                set_rgb(1, 1, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }

            case LED_STATE_CHARGING: {
                // 使用PWM实现更平滑的呼吸效果
                uint8_t duty = increasing ? brightness : (100 - brightness);
                if (get_red_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, (255 * duty) / 100);
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, 255);  // 关闭
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, 255);   // 关闭
                }
                
                brightness = increasing ? (brightness + 2) : (brightness - 2);
                if (brightness >= 100) {
                    increasing = false;
                } else if (brightness <= 0) {
                    increasing = true;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
            }

            case LED_STATE_FULL_BATTERY: {
                // 绿灯常亮
                set_rgb(1, 0, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }

            case LED_STATE_USER_SPEAKING: {
                set_rgb(1, 1, 0);
                break;
            }

            case LED_STATE_AI_SPEAKING: {
                set_rgb(1, 0, 1);
                break;
            }

            case LED_STATE_POOR_NETWORK: {
                uint8_t duty = increasing ? brightness : (100 - brightness);
                uint8_t red_duty = (255 * duty) / 100;     // 红色保持原来的亮度
                uint8_t green_duty = (255 * duty * 0.6) / 100;  // 绿色降低到60%亮度
                
                // 设置红色和绿色LED的亮度，关闭蓝色LED
                if (get_red_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, red_duty);      // 红色全量
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, green_duty);  // 绿色降低亮度
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, 255);         // 蓝色关闭
                }
                
                // 更新所有通道
                if (get_red_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN);
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE);
                }
                
                // 更新亮度值
                brightness = increasing ? (brightness + 2) : (brightness - 2);
                if (brightness >= 100) {
                    increasing = false;
                } else if (brightness <= 0) {
                    increasing = true;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
            }

            case LED_STATE_STANDBY: {
                // 使用PWM实现绿灯呼吸效果
                uint8_t duty = increasing ? brightness : (100 - brightness);
                if (get_red_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_RED, 255);  // 关闭
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_GREEN, (255 * duty) / 100);
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_BLUE, 255);  // 关闭
                }
                
                if (get_red_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_RED);
                }
                if (get_green_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_GREEN);
                }
                if (get_blue_led_gpio() >= 0) {
                    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_BLUE);
                }
                
                brightness = increasing ? (brightness + 2) : (brightness - 2);
                if (brightness >= 100) {
                    increasing = false;
                } else if (brightness <= 0) {
                    increasing = true;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                break;
            }

            default:
                led_task_stop();
                return;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 修改 led_set_state 函数
void led_set_state(led_state_t state) {

#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    return;
#endif

    static bool initialized = false;
#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1)
    switch (state) {
        case LED_STATE_STANDBY:

            led_effect_msg_t msg3 = {
                .effect = LED_EFFECT_SOLID,
                .config.color = {
                    .r = rgb_r_brightness,
                    .g = rgb_g_brightness,
                    .b = rgb_b_brightness
                }
            };
            led_effect_start(&msg3);
            
            break;
        case LED_STATE_WIFI_CONNECTING: {
            led_effect_msg_t msg = {
                .effect = LED_EFFECT_MARQUEE,
                .config.marquee = {
                    .r = 0,
                    .g = 0,
                    .b = 255,
                    .start_pos = 0,
                    .rounds = 3000,
                    .delay_ms = 200
                }
            };
            ESP_LOGI(TAG,"LED_STATE_WIFI_CONNECTING");
            led_effect_start(&msg);
            break;
        }
        case LED_STATE_AGING: {
            led_effect_msg_t msg = {
                .effect = LED_EFFECT_MARQUEE,
                .config.marquee = {
                    .r = 255,
                    .g = 255,
                    .b = 0,
                    .start_pos = 0,
                    .rounds = -1,
                    .delay_ms = 1000
                }
            };
            ESP_LOGI(TAG,"LED_STATE_WIFI_CONNECTING");
            led_effect_start(&msg);
            break;
        }
        case LED_STATE_WIFI_CONNECTED: {
            // run_heart_beat(3000);
            break;
        }
        case LED_STATE_POOR_NETWORK: {
            led_effect_msg_t msg = {
                .effect = LED_EFFECT_MARQUEE,
                .config.marquee = {
                    .r = 255,
                    .g = 0,
                    .b = 0,
                    .start_pos = 0,
                    .rounds = 20,
                    .delay_ms = 200
                }
            };
            led_effect_start(&msg);
            break;
        }
        case LED_STATE_NO_CONNECT_WIFI: {
            led_effect_msg_t msg3 = {
                .effect = LED_EFFECT_SOLID,
                .config.color = {
                    .r = 255,
                    .g = 0,
                    .b = 0
                }
            };
            led_effect_start(&msg3);
            break;
        }
        case LED_STATE_INIT:
        case LED_STATE_RESET:
            led_effect_msg_t msg1 = {
                .effect = LED_EFFECT_MARQUEE,
                .config.marquee = {
                    .r = 255,
                    .g = 255,
                    .b = 255,
                    .start_pos = 0,
                    .rounds = -1,
                    .delay_ms = 200
                }
            };
            led_effect_start(&msg1);
            break;
        case LED_STATE_USER_SPEAKING:
            led_effect_msg_t msg2 = {
                .effect = LED_EFFECT_SOLID,
                .config.color = {
                    .r = 0,
                    .g = 0,
                    .b = 255
                }
            };
            led_effect_start(&msg2);
            break;
        case LED_STATE_AI_SPEAKING:
            led_effect_msg_t msg = {
                .effect = LED_EFFECT_SOLID,
                .config.color = {
                    .r = 0,
                    .g = 255,
                    .b = 0
                }
            };
            led_effect_start(&msg);
            break;
        case LED_STATE_SLEEPING:
            run_blue_heart_beat(3000);
            break;
        // case LED_STATE_TEST_LED:
        //     run_test_led();
        //     break;
        default:
            break;
    }
    if (!initialized) {
        led_manager_init();
        xTaskCreatePinnedToCore(led_manager_task, 
                   "led_manager", 
                   1024 * 1,
                   NULL, 
                   4,  // 比LED控制任务优先级高
                   &led_manager_task_handle,
                   0);
        initialized = true;
    }
    // 发送状态到队列
    xQueueSend(led_queue, &state, 0);
#endif
}

// 修改 led_task_stop 函数
void led_task_stop(void) {
    led_state_t state = LED_STATE_OFF;
    xQueueSend(led_queue, &state, 0);
}


void init_pa(void) {
    // printf("gpio init pa enable: %d\n", get_pa_enable_gpio());
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#else
    gpio_drive_init(GPIO_MODE_OUTPUT, 0, 0, GPIO_INTR_DISABLE, get_pa_enable_gpio(), 1);
#endif
}
void gpio_set_power_status(int status)
{
    // IO15
    ESP_LOGW("SLEEP", "gpio_set_power_status: %d", status);
    rtc_gpio_hold_dis(get_power_hold_gpio());
    vTaskDelay(pdMS_TO_TICKS(500));  // 延时500毫秒
    gpio_drive_init(GPIO_MODE_OUTPUT, 0, 0, GPIO_INTR_DISABLE, get_power_hold_gpio(), status);
}
void gpio_set_pa_enable(uint8_t status)
{
    // printf("gpio set pa enable: %d to %d\n", get_pa_enable_gpio(), status);
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#else
    gpio_set_status(get_pa_enable_gpio(), status);
#endif
}

// PWM初始化函数
static void pwm_init(void) {
    // 配置LEDC定时器
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER
    };
    ledc_timer_config(&ledc_timer);

    // 配置LEDC通道
    ledc_channel_config_t ledc_channel[3] = {
        {   // 红色LED通道
            .channel    = LEDC_CHANNEL_RED,
            .duty       = 0,
            .gpio_num   = get_red_led_gpio(),
            .speed_mode = LEDC_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER
        },
        {   // 绿色LED通道
            .channel    = LEDC_CHANNEL_GREEN,
            .duty       = 0,
            .gpio_num   = get_green_led_gpio(),
            .speed_mode = LEDC_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER
        },
        {   // 蓝色LED通道
            .channel    = LEDC_CHANNEL_BLUE,
            .duty       = 0,
            .gpio_num   = get_blue_led_gpio(),
            .speed_mode = LEDC_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER
        }
    };

    // 配置每个LED通道
    for (int i = 0; i < 3 && ledc_channel[i].gpio_num >= 0; i++) {
        if (ledc_channel[i].gpio_num >= 0) {
            ledc_channel_config(&ledc_channel[i]);
        }
    }
}

#else
// 实现空方法
void gpio_set_power_status(int status) {}
void gpio_set_pa_enable(uint8_t status) {}
void gpio_set_status(uint32_t pinNumber,uint8_t status) {}
void gpio_drive_init(gpio_mode_t gpioMode, gpio_pullup_t pullUpEnable, gpio_pulldown_t pullDownEnable, gpio_int_type_t interruptType, uint32_t pinNumber, uint32_t initialLevel) {}
void gpio_init(void) {}
void gpio_set_blue_led(uint8_t status) {}
void gpio_set_red_led(uint8_t status) {}
void gpio_set_green_led(uint8_t status) {}
void led_set_state(led_state_t state) {}
void led_task_stop(void) {}
void init_pa(void) {}
#endif


#define CONFIG_BLINK_PERIOD     1000  // ms

static uint8_t s_led_state = 1;
static led_strip_handle_t led_strip;
static TaskHandle_t led_effect_task_handle = NULL;
static QueueHandle_t led_effect_queue = NULL;
static bool led_effect_running = false;
static volatile bool effect_should_stop = false;

static void led_effect_task(void *arg);  // 函数前向声明

// 添加 HSV 到 RGB 转换的辅助函数
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = v - c;
    
    float r_temp, g_temp, b_temp;
    if (h >= 0 && h < 60) {
        r_temp = c; g_temp = x; b_temp = 0;
    } else if (h >= 60 && h < 120) {
        r_temp = x; g_temp = c; b_temp = 0;
    } else if (h >= 120 && h < 180) {
        r_temp = 0; g_temp = c; b_temp = x;
    } else if (h >= 180 && h < 240) {
        r_temp = 0; g_temp = x; b_temp = c;
    } else if (h >= 240 && h < 300) {
        r_temp = x; g_temp = 0; b_temp = c;
    } else {
        r_temp = c; g_temp = 0; b_temp = x;
    }
    
    *r = (uint8_t)((r_temp + m) * 255);
    *g = (uint8_t)((g_temp + m) * 255);
    *b = (uint8_t)((b_temp + m) * 255);
}

// 跑马灯效果
void led_strip_marquee(marquee_config_t config) 
{
    uint8_t current_pos;
    uint8_t round = 0;
    effect_should_stop = false;
    
    while(!effect_should_stop && (config.rounds < 0 || round < config.rounds)) {
        // 如果是-1，则一直运行
        // 清除所有LED
        led_strip_clear(led_strip);
        
        // 计算当前位置
        current_pos = (config.start_pos + round) % MAX_LED_NUM;
        
        // 设置当前LED
        led_strip_set_pixel(led_strip, current_pos, 
                          apply_brightness(config.r), 
                          apply_brightness(config.g), 
                          apply_brightness(config.b));
        
        // 刷新显示
        led_strip_refresh(led_strip);
        
        // 延时
        vTaskDelay(pdMS_TO_TICKS(config.delay_ms));
        
        round++;
    }
    // 结束之后回到呼吸
    bool is_connected_wifi = get_wifi_is_connected();
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
    if (is_connected_wifi == false) {
        // 红色
        led_set_state(LED_STATE_NO_CONNECT_WIFI);
    } else {
        led_set_state(LED_STATE_STANDBY);
    }

}

void led_strip_breath(breath_config_t config)
{
    const int steps = 40;
    float brightness;
    int count = 0;
    effect_should_stop = false;
    
    // 定义七色彩虹的HSV色相值
    const float rainbow_hues[] = {
        0.0f,    // 赤 (红)
        30.0f,   // 橙
        60.0f,   // 黄
        120.0f,  // 绿
        180.0f,  // 青
        240.0f,  // 蓝
        270.0f   // 紫
    };
    const int num_colors = sizeof(rainbow_hues) / sizeof(rainbow_hues[0]);
    int color_index = 0;
    
    // 验证亮度范围
    config.min_bright = (config.min_bright < 0.0f) ? 0.0f : 
                       (config.min_bright > 1.0f) ? 1.0f : config.min_bright;
    config.max_bright = (config.max_bright < 0.0f) ? 0.0f : 
                       (config.max_bright > 1.0f) ? 1.0f : config.max_bright;
    if(config.max_bright < config.min_bright) {
        float temp = config.max_bright;
        config.max_bright = config.min_bright;
        config.min_bright = temp;
    }
    
    while(!effect_should_stop && (config.times < 0 || count < config.times)) {
        // 获取当前颜色的色相值
        float current_hue = rainbow_hues[color_index];
        

        // global_led_brightness 作为最大亮度
        float brightness_range = global_led_brightness / 100.0;
        // 从暗到亮

        for(int i = 0; i <= steps; i++) {
            if(effect_should_stop) break;
            brightness = config.min_bright + 
                        (brightness_range * (float)i / steps);
            
            // 计算当前颜色
            uint8_t r, g, b;
            hsv_to_rgb(current_hue, 1.0f, 1.0f, &r, &g, &b);
            
            // 设置所有LED的亮度和颜色
            for(int j = 0; j < MAX_LED_NUM; j++) {
                led_strip_set_pixel(led_strip, j,
                                  r * brightness,
                                  g * brightness, 
                                  b * brightness);
            }
            
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(config.period_ms/(steps * 2)));
        }
        if(effect_should_stop) break;
        
        // 从亮到暗
        for(int i = steps; i >= 0; i--) {
            if(effect_should_stop) break;
            brightness = config.min_bright + 
                        (brightness_range * (float)i / steps);
            
            // 计算当前颜色
            uint8_t r, g, b;
            hsv_to_rgb(current_hue, 1.0f, 1.0f, &r, &g, &b);
            
            // 设置所有LED的亮度和颜色
            for(int j = 0; j < MAX_LED_NUM; j++) {
                led_strip_set_pixel(led_strip, j,
                                  r * brightness,
                                  g * brightness, 
                                  b * brightness);
            }
            
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(config.period_ms/(steps * 2)));
        }
        
        // 切换到下一种颜色
        color_index = (color_index + 1) % num_colors;
        
        // 完成一轮七色后计数增加
        if(color_index == 0) {
            count++;
        }
    }
    
    // 结束时清除所有LED
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

// void led_strip_test_led(uint8_t r, uint8_t g, uint8_t b)
// {
//     static uint8_t current_color = 0;
//     led_color_t colors[] = {
//         {r, 0, 0},
//         {0, g, 0},
//         {0, 0, b},
//         {r, g, b}
//     };

//     int color_count = sizeof(colors) / sizeof(colors[0]);
//     if (current_color >= color_count) {
//         current_color = 0;
//     }

//     led_strip_clear(led_strip);
//     for(int i = 0; i < 4; i++) {
//         led_strip_set_pixel(led_strip, i, apply_brightness(colors[current_color].r), apply_brightness(colors[current_color].g), apply_brightness(colors[current_color].b));
//     }
//     led_strip_refresh(led_strip);

//     current_color++;
// }

// void led_strip_update(uint8_t state)
// {
//     if (state) {
//         // 使用新的效果函数
//         marquee_config_t config = {
//             .r = 80,
//             .g = 0, 
//             .b = 0,
//             .start_pos = 0,
//             .rounds = 4,
//             .delay_ms = 100
//         };
//         led_strip_marquee(config);
//     } else {
//         led_strip_clear(led_strip);
//     }
// }

void led_strip_init(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED!");
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = MAX_LED_NUM,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
    
    // 创建LED效果消息队列
    led_effect_queue = xQueueCreate(20, sizeof(led_effect_msg_t));
    
    // 创建LED效果控制任务
    xTaskCreate(led_effect_task, "led_effect", 4096, NULL, 5, &led_effect_task_handle);
}


void led_strip_blue_breath(void) {
    float brightness = 0.0f;
    bool increasing = true;
    
    while (!effect_should_stop) {
        // 计算当前亮度
        if (increasing) {
            brightness += 0.01f;
            if (brightness >= global_led_brightness) {
                brightness = global_led_brightness;
                increasing = false;
            }
        } else {
            brightness -= 0.01f;
            if (brightness <= 0.0f) {
                brightness = 0.0f;
                increasing = true;
            }
        }
        
        // 设置蓝色 LED
        for (int i = 0; i < MAX_LED_NUM; i++) {
            led_strip_set_pixel(led_strip, i, 0, 0, (uint8_t)(brightness * 255));
        }
        led_strip_refresh(led_strip);
        
        // 控制呼吸速度
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    
    // 清除LED效果
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

static void led_effect_task(void *arg)
{
    led_effect_msg_t msg;
    
    while (1) {
        if (xQueueReceive(led_effect_queue, &msg, portMAX_DELAY)) {
            led_effect_running = true;
            effect_should_stop = false;  // 重置停止标志
            
            switch (msg.effect) {
                case LED_EFFECT_BREATH:
                    led_strip_breath(msg.config.breath);
                    break;
                case LED_STATE_BLUE_BREATH:
                    led_strip_blue_breath();  // 直接调用蓝色呼吸效果
                    break;
                    
                case LED_EFFECT_MARQUEE:
                    led_strip_marquee(msg.config.marquee);
                    break;

                case LED_EFFECT_STOP:
                    led_strip_clear(led_strip);
                    led_strip_refresh(led_strip);
                    led_strip_solid_color(0, 0, 0);
                    break;
                    
                // case LED_EFFECT_TEST_LED:
                //     led_strip_clear(led_strip);
                //     led_strip_refresh(led_strip);
                //     led_strip_test_led(msg.config.color.r, msg.config.color.g, msg.config.color.b);
                //     break;
                    
                case LED_EFFECT_SOLID:
                    led_strip_clear(led_strip);
                    led_strip_refresh(led_strip);
                    led_strip_solid_color(msg.config.color.r, 
                                        msg.config.color.g, 
                                        msg.config.color.b);
                    break;
                    
                default:
                    break;
            }
            
            led_effect_running = false;
        }
    }
}

void led_effect_start(led_effect_msg_t *msg)
{
    if (led_effect_queue) {
        // 设置停止标志
        led_effect_stop();
        vTaskDelay(pdMS_TO_TICKS(1));
        // 发送新效果
        xQueueSend(led_effect_queue, msg, 0);

    }
}

void led_effect_stop(void)
{
    if (led_effect_queue) {
        effect_should_stop = true;
        // 等待当前效果停止
        vTaskDelay(pdMS_TO_TICKS(100));
        led_effect_msg_t msg = {
            .effect = LED_EFFECT_STOP
        };
        xQueueSend(led_effect_queue, &msg, 0);
    }
}



void run_heart_beat(int period_ms) {
    led_effect_msg_t msg = {
        .effect = LED_EFFECT_BREATH,
        .config.breath = {
            .r = 255,
            .g = 255,
            .b = 255,
            .period_ms = period_ms,
            .times = -1,
            .min_bright = 0.0f,
            .max_bright = 1.0f
        }
    };
    led_effect_start(&msg);
}

void run_blue_heart_beat(int period_ms) {
    led_effect_msg_t msg = {
        .effect = LED_STATE_BLUE_BREATH,
        .config.breath = {
            .r = 0,
            .g = 0,
            .b = 255,
            .period_ms = period_ms,
            .times = -1,
            .min_bright = 0.0f,
            .max_bright = 1.0f
        }
    };
    led_effect_start(&msg);
}

uint8_t key_to_led_position(int key_id)
{
    switch (key_id) {
        case INPUT_KEY_USER_ID_PLAY:
            return LED_POS_PLAY;
        case INPUT_KEY_USER_ID_VOLUP:
            return LED_POS_VOLUP;
        case INPUT_KEY_USER_ID_VOLDOWN:
            return LED_POS_VOLDOWN;
        case INPUT_KEY_USER_ID_REC:
            return LED_POS_REC;
        default:
            return 0;
    }
}

// 获取按键对应的LED颜色
led_color_t get_key_led_color(int key_id)
{
    led_color_t color;
    
    switch (key_to_led_position(key_id)) {
        case LED_POS_REC:      // 录音键 - 玫瑰金色
            color.r = 255;
            color.g = 0;
            color.b = 0;
            break;
        case LED_POS_PLAY:     // 播放键 - 天蓝色
            color.r = 0;
            color.g = 255;
            color.b = 0;
            break;
        case LED_POS_VOLUP:    // 音量加 - 薄荷绿
            color.r = 0;
            color.g = 0;
            color.b = 255;
            break;
        case LED_POS_VOLDOWN:  // 音量减 - 淡紫色
            color.r = 255;
            color.g = 255;
            color.b = 255;
            break;
        default:               // 默认白色
            color.r = 255;
            color.g = 255;
            color.b = 255;
            break;
    }
    
    return color;
}

// 添加长亮效果实现函数
void led_strip_solid_color(uint8_t r, uint8_t g, uint8_t b) {
    // Apply global brightness to each color component
   
    
    // Set all LEDs to the adjusted color
    
    bool is_white = r == 255 && g == 255 && b == 255;
    float coefficient = is_white ? 0.1 : 1.0;
    // Keep color until interrupted
    uint8_t adjusted_r = apply_brightness(r) * coefficient;
    uint8_t adjusted_g = apply_brightness(g) * coefficient;
    uint8_t adjusted_b = apply_brightness(b) * coefficient;
    for (int i = 0; i < MAX_LED_NUM; i++) {
        led_strip_set_pixel(led_strip, i, adjusted_r, adjusted_g, adjusted_b);
    }
    led_strip_refresh(led_strip);
    // while (!effect_should_stop) {
    //     vTaskDelay(pdMS_TO_TICKS(10));
    // }
    // ESP_LOGE(TAG, "led_strip_solid_color end");
    // // Clear LED effect
    // led_strip_clear(led_strip);
    // led_strip_refresh(led_strip);
}