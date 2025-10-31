#include <stdio.h>
#include "tt_ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include <math.h>
#include "board/charge.h"
#include "gizwits_protocol.h"
#include "battery.h"



#define BLINK_GPIO              GPIO_NUM_2
#define BLINK_MAX_LEDS          32

static led_strip_handle_t       led_strip = NULL;
static TaskHandle_t             led_effect_task_handle = NULL;// 效果队列
static QueueHandle_t            led_effect_queue = NULL;
static bool                     led_effect_running = false;
static volatile bool            effect_should_stop = false; // true：退出当前灯光效果
static float global_led_brightness   = 100.0f;

static const char* state_names[TT_LED_STATE_MAX] = {
    "OFF", 
    "PURPLE", 
    "RED", 
    "ORANGE", 
    "WHITE", 
    "GREEN", 
    "BLUE", 
    "YELLOW", 
};

static const char *TAG = "TT_LED";

static void tt_led_effect_task(void *arg);
void tt_led_strip_off(void);
void tt_led_strip_flow(flow_config_t config);
void tt_led_strip_marquee(tt_marquee_config_t config);
void tt_led_strip_breath(tt_breath_config_t config);
void tt_led_strip_none(none_config_t config);
// static StackType_t led_effect_task_stack[4096];

void tt_led_strip_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = BLINK_MAX_LEDS,
    };
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // vTaskDelay(pdMS_TO_TICKS(1000));
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);

    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // 创建LED效果消息队列
    led_effect_queue = xQueueCreate(20, sizeof(tt_led_effect_msg_t));
    
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // 创建LED效果控制任务
    // INSERT_YOUR_REWRITE_HERE
    // static StaticTask_t led_effect_task_buffer;

    
    led_effect_task_handle = xTaskCreateExt(
        tt_led_effect_task, 
        "led_effect", 
        4096, 
        NULL, 
        5, 
        NULL
    );
    
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    // vTaskDelay(pdMS_TO_TICKS(1000));
}

void tt_led_effect_stop(void)
{
    if (led_effect_queue) {
        effect_should_stop = true;
        // 等待当前效果停止
        vTaskDelay(pdMS_TO_TICKS(100));
        tt_led_effect_msg_t msg = {
            .effect = TT_LED_EFFECT_STOP
        };
        xQueueSend(led_effect_queue, &msg, 0);
    }
}

void tt_led_effect_start(tt_led_effect_msg_t *msg)
{
    if (led_effect_queue) {
        // 设置停止标志
        tt_led_effect_stop();
        vTaskDelay(pdMS_TO_TICKS(1));
        // 发送新效果
        xQueueSend(led_effect_queue, msg, 0);

    }
}
// Start of Selection
static tt_led_state_t last_state = TT_LED_STATE_MAX; // 初始化上一次状态
tt_led_state_t get_tt_led_last_state(void)
{
    return last_state;
}

// 设置 tt_led_strip_set_state 函数
void __tt_led_strip_set_state(const char * func, uint32_t line, tt_led_state_t state)
{

    ESP_LOGI(TAG, "%s:new[%s], old[%s], by %s:%d", __func__, state_names[state], 
            last_state<TT_LED_STATE_MAX?state_names[last_state]:"NOT INIT", func, line);
    
    if(get_valuestate() == state_VALUE0_close)
    {
        state = TT_LED_STATE_OFF;
        ESP_LOGI(TAG, "%s:%d, state close , change to off", __func__, __LINE__, battery_get_estimate(TYPE_AVERAGE));
    }
    else
    {
        // 增加电量低强制灯红色
        if(battery_get_estimate(TYPE_AVERAGE) <= RED_ON_PERCENTAGE && get_battery_state() == BATTERY_NOT_CHARGING )
        {
            ESP_LOGI(TAG, "%s:%d, battery_percentage: %d, change to red", __func__, __LINE__, battery_get_estimate(TYPE_AVERAGE));
            state = TT_LED_STATE_RED;
        }
    }

    
    // 如果当前状态与上一次状态相同，则不处理
    if (last_state != TT_LED_STATE_MAX && state == last_state) {
        ESP_LOGI(TAG, "State unchanged, no action taken.");
        // return;
    }

    // 更新上一次状态
    last_state = state;
    switch (state)
    {
    case TT_LED_STATE_OFF: // 关闭灯光
        tt_led_effect_msg_t msg = {
            .effect = TT_LED_EFFECT_STOP};
        ESP_LOGI(TAG, "LED_STATE_OFF");
        tt_led_effect_start(&msg);
        break;
    case TT_LED_STATE_PURPLE: // 效果一，紫色LED呼吸灯
        tt_led_effect_msg_t msg1 = {
            .effect = TT_LED_EFFECT_BREATH,
            .config.breath = {
                .hr = 128,
                .hg = 0,
                .hb = 150,
                .lr = 40,
                .lg = 0,
                .lb = 40,
                .period_ms = 3000,
                .times = -1,
                .min_bright = 0.0f,
                .max_bright = 1.0f}};
        ESP_LOGI(TAG, "LED_STATE_Purple");
        tt_led_effect_start(&msg1);
        break;

    case TT_LED_STATE_RED: // 效果二，红色LED呼吸灯
        tt_led_effect_msg_t msg2 = {
            .effect = TT_LED_EFFECT_BREATH,
            .config.breath = {
                .hr = 255,
                .hg = 0,
                .hb = 0,
                .lr = 50,
                .lg = 0,
                .lb = 0,
                .period_ms = 1000,
                .times = -1,
                .min_bright = 0.0f,
                .max_bright = 1.0f}};
        ESP_LOGI(TAG, "LED_STATE_Red");
        tt_led_effect_start(&msg2);
        break;

    case TT_LED_STATE_ORANGE: // 效果三，橙色LED呼吸灯
        tt_led_effect_msg_t msg3 = {
            .effect = TT_LED_EFFECT_BREATH,
            .config.breath = {
                .hr = 255,
                .hg = 50,
                .hb = 0,
                .lr = 75,
                .lg = 16,
                .lb = 0,
                .period_ms = 3000,
                .times = -1,
                .min_bright = 0.0f,
                .max_bright = 1.0f}};
        ESP_LOGI(TAG, "LED_STATE_Orange");
        tt_led_effect_start(&msg3);
        break;
    case TT_LED_STATE_WHITE: // 效果4，白色常亮
        tt_led_effect_msg_t msg4 = {
            .effect = TT_LED_EFFECT_NONE,
            .config.none = {
                .r = 0xff,
                .g = 145,// 182
                .b = 78, // 157
            }};
        ESP_LOGI(TAG, "LED_STATE_Write");
        tt_led_effect_start(&msg4);
        break;

    case TT_LED_STATE_GREEN: // 效果4，白色常亮
        tt_led_effect_msg_t msg5 = {
            .effect = TT_LED_EFFECT_NONE,
            .config.none = {
                .r = 0,
                .g = 255,
                .b = 0,
            }};
        ESP_LOGI(TAG, "LED_STATE_Green");
        tt_led_effect_start(&msg5);
        break;
    case TT_LED_STATE_BLUE: // 效果6，蓝色常亮
        tt_led_effect_msg_t msg6 = {
            .effect = TT_LED_EFFECT_NONE,
            .config.none = {
                .r = 0,
                .g = 0,
                .b = 255,
            }};
        ESP_LOGI(TAG, "LED_STATE_Blue");
        tt_led_effect_start(&msg6);
        break;
    case TT_LED_STATE_YELLOW: // 效果7，黄色常亮
        tt_led_effect_msg_t msg7 = {
            .effect = TT_LED_EFFECT_NONE,
            .config.none = {
                .r = 255,
                .g = 255,
                .b = 0,
            }};
        ESP_LOGI(TAG, "LED_STATE_Yellow");
        tt_led_effect_start(&msg7);
        break;
    case TT_LED_STATE_MAX:
        break;
    default:
        break;
    }
}

void show_led_msg(tt_led_effect_msg_t msg)
{
    ESP_LOGI(TAG, "Received message effect: %d", msg.effect);
    switch (msg.effect) {
        case TT_LED_EFFECT_BREATH:
            ESP_LOGI(TAG, "Breath config - hr: %d, hg: %d, hb: %d, lr: %d, lg: %d, lb: %d, period_ms: %d, times: %d, min_bright: %f, max_bright: %f",
                        msg.config.breath.hr, msg.config.breath.hg, msg.config.breath.hb,
                        msg.config.breath.lr, msg.config.breath.lg, msg.config.breath.lb,
                        msg.config.breath.period_ms, msg.config.breath.times,
                        msg.config.breath.min_bright, msg.config.breath.max_bright);
            break;
        case TT_LED_EFFECT_MARQUEE:
            ESP_LOGI(TAG, "Marquee config - r: %d, g: %d, b: %d, start_pos: %d, rounds: %d, delay_ms: %d",
                        msg.config.marquee.r, msg.config.marquee.g, msg.config.marquee.b,
                        msg.config.marquee.start_pos, msg.config.marquee.rounds, msg.config.marquee.delay_ms);
            break;
        case TT_LED_EFFECT_FLOW:
            ESP_LOGI(TAG, "Flow config - r: %d, g: %d, b: %d, period_ms: %d, delay_ms: %d",
                        msg.config.flow.r, msg.config.flow.g, msg.config.flow.b,
                        msg.config.flow.period_ms, msg.config.flow.delay_ms);
            break;
        case TT_LED_EFFECT_NONE:
            ESP_LOGI(TAG, "None config - r: %d, g: %d, b: %d",
                        msg.config.none.r, msg.config.none.g, msg.config.none.b);
            break;
        default:
            ESP_LOGI(TAG, "Unknown effect");
            break;
    }
}
static void tt_led_effect_task(void *arg)
{
    tt_led_effect_msg_t msg;
    while (1)
    {
        if (xQueueReceive(led_effect_queue, &msg, portMAX_DELAY))
        {
            show_led_msg(msg);
            led_effect_running = true;
            effect_should_stop = false; // 重置停止标志
            switch (msg.effect)
            {
            case TT_LED_EFFECT_STOP: // 关闭灯光
                tt_led_strip_off();
                break;
            case TT_LED_EFFECT_BREATH:// 呼吸灯效果
                tt_led_strip_breath(msg.config.breath);
                break;
            case TT_LED_EFFECT_MARQUEE: // 跑马灯效果
                tt_led_strip_marquee(msg.config.marquee);
                break;
            case TT_LED_EFFECT_FLOW: // 流水灯光
                tt_led_strip_flow(msg.config.flow);
                break;
            case TT_LED_EFFECT_NONE: // 常亮灯光
                tt_led_strip_none(msg.config.none);
                break;
            default:
                break;
            }
        }
        led_effect_running = false;
    }
}

// 关闭灯
void tt_led_strip_off(void) 
{
    led_strip_clear(led_strip);
    for (int i = 0; i < BLINK_MAX_LEDS; i++) {
        led_strip_set_pixel(led_strip, i, 0, 0, 0);
    }
    led_strip_refresh(led_strip);
}

// 常亮灯效果
void tt_led_strip_none(none_config_t config)
{
    for (int j = 0; j < BLINK_MAX_LEDS; j++)
    {
        led_strip_set_pixel(led_strip, j, config.r, config.g, config.b);
    }
    led_strip_refresh(led_strip);
}

// 流水灯效果
void tt_led_strip_flow(flow_config_t config) 
{
    led_strip_refresh(led_strip);
}

// 跑马灯效果
void tt_led_strip_marquee(tt_marquee_config_t config) 
{
    led_strip_refresh(led_strip);
}

// 呼吸灯效果
void tt_led_strip_breath(tt_breath_config_t config)
{
    const int steps = 100;
    float brightness;
    int64_t count = 0;
    effect_should_stop = false;

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
        // global_led_brightness 作为最大亮度
        float brightness_range = global_led_brightness / 100.0;
        // 从暗到亮

        // 计算出最小亮度对应的开始值
        int start_step = 0;

        for(int i = 0; i <= steps; i++) {
            if(effect_should_stop) break;
            brightness = config.min_bright + 
                        (brightness_range * (float)i / steps);
            
            if (config.hr * brightness <= config.lr &&
                config.hg * brightness <= config.lg &&
                config.hb * brightness <= config.lb)
            {
                start_step = i;
                continue;
            }
            // 设置所有LED的亮度和颜色
            for(int j = 0; j < BLINK_MAX_LEDS; j++) {
                led_strip_set_pixel(led_strip, j,
                                  config.hr * brightness,
                                  config.hg * brightness, 
                                  config.hb * brightness);
            }
            
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(config.period_ms / ((steps - start_step) * 2)));
        }
        if(effect_should_stop) break;
        
        // 从亮到暗
        for(int i = steps; i >= start_step; i--) {
            if(effect_should_stop) break;
            brightness = config.min_bright + 
                        (brightness_range * (float)i / steps);
                        
            // 设置所有LED的亮度和颜色
            for(int j = 0; j < BLINK_MAX_LEDS; j++) {
                led_strip_set_pixel(led_strip, j,
                                    config.hr * brightness > config.lr ? config.hr * brightness : config.lr,
                                    config.hg * brightness > config.lg ? config.hg * brightness : config.lg,
                                    config.hb * brightness > config.lb ? config.hb * brightness : config.lb);
                if (config.hr * brightness <= config.lr &&
                    config.hg * brightness <= config.lg &&
                    config.hb * brightness <= config.lb)
                {
                    break;
                }
            }
            
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(config.period_ms/((steps - start_step) * 2)));
        }
        count++;
    }
    
    // 结束时清除所有LED
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}