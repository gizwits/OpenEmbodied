#include "touch_key.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/touch_pad.h"
#include "driver/touch_sensor.h"
#include "esp_log.h"
#include "driver/touch_sensor_common.h"

static const char* TAG = "TOUCH_KEY";

static touch_callback_t touch_callback = NULL;

// 触摸按键配置信息
static const touch_pad_config_t pad_configs[] = {
    {
        .pad_num = TOUCH_PAD_NUM1,  // 使用GPIO8作为触摸输入
        .thresh = 2000,              // 触发阈值
        .long_press_time = 2000     // 长按时间2秒
    },
    {
        .pad_num = TOUCH_PAD_NUM2,  // 使用GPIO9作为触摸输入
        .thresh = 2000,              // 触发阈值
        .long_press_time = 2000     // 长按时间2秒
    }
};

// 触摸按键状态信息
static touch_pad_state_info_t pad_states[2] = {
    {
        .state = TOUCH_IDLE,
        .press_start = 0,
        .last_press = 0
    },
    {
        .state = TOUCH_IDLE,
        .press_start = 0,
        .last_press = 0
    }
};

// 回调函数示例
void touch_event_callback(touch_pad_evt_t evt) {
    switch (evt) {
        case TOUCH_EVT_PRESS:
            printf("Touch pad pressed\n");
            break;
        case TOUCH_EVT_RELEASE:
            printf("Touch pad released\n");
            break;
        case TOUCH_EVT_LONG_PRESS:
            printf("Touch pad long pressed\n");
            break;
        default:
            printf("Unknown touch event\n");
            break;
    }
}

// 注册触摸事件回调函数
void touch_pad_register_callback(touch_callback_t cb) {
    ESP_LOGI(TAG, "Registering touch pad callback");
    touch_callback = cb;
}

// 触摸按键初始化
void touch_pad_init_2(void) {
    ESP_LOGI(TAG, "Initializing touch pads for ESP32-S3");
    
    // 设置触摸参考电压
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    
    // 配置指定触摸通道
    for (int i = 0; i < 2; i++) {
        ESP_LOGI(TAG, "Configuring touch pad %d", pad_configs[i].pad_num);
        ESP_ERROR_CHECK(touch_pad_config(pad_configs[i].pad_num));
        // 设置触摸阈值
        ESP_ERROR_CHECK(touch_pad_set_thresh(pad_configs[i].pad_num, pad_configs[i].thresh));
    }
    
    // 启动触摸传感器 FSM
    ESP_ERROR_CHECK(touch_pad_fsm_start());
    ESP_LOGI(TAG, "Touch pads initialized for ESP32-S3");
}

// 触摸按键处理任务
static void touch_pad_read_task(void *pvParameter) {
    uint16_t touch_value;
    uint32_t current_time;
    
    while (1) {
        current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        for (int i = 0; i < 2; i++) {
            ESP_ERROR_CHECK(touch_pad_filter_read_smooth(pad_configs[i].pad_num, &touch_value));
            ESP_LOGW(TAG, "Touch pad %d value: %d", pad_configs[i].pad_num, touch_value);
            
            // 检测触摸状态
            if (touch_value < pad_configs[i].thresh) {  // 触摸被按下
                switch (pad_states[i].state) {
                    case TOUCH_IDLE:
                        pad_states[i].state = TOUCH_PRESS;
                        pad_states[i].press_start = current_time;
                        ESP_LOGI(TAG, "Touch pad %d pressed", pad_configs[i].pad_num);
                        if (touch_callback) {
                            touch_callback(TOUCH_EVT_PRESS);
                        }
                        break;
                        
                    case TOUCH_PRESS:
                        if ((current_time - pad_states[i].press_start) >= pad_configs[i].long_press_time) {
                            pad_states[i].state = TOUCH_LONG_PRESS;
                            ESP_LOGI(TAG, "Touch pad %d long pressed", pad_configs[i].pad_num);
                            if (touch_callback) {
                                touch_callback(TOUCH_EVT_LONG_PRESS);
                            }
                        }
                        break;
                        
                    default:
                        break;
                }
            } else {  // 触摸释放
                if (pad_states[i].state != TOUCH_IDLE) {
                    pad_states[i].state = TOUCH_IDLE;
                    ESP_LOGI(TAG, "Touch pad %d released", pad_configs[i].pad_num);
                    if (touch_callback) {
                        touch_callback(TOUCH_EVT_RELEASE);
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // 采样间隔
    }
}

// 触摸模块启动函数
void touch_pad_start(void) {
    ESP_LOGI(TAG, "Starting touch pad module for ESP32-S3");
    // 初始化触摸模块

    touch_pad_register_callback(touch_event_callback);

    touch_pad_init_2();
    
    // 创建触摸检测任务
    xTaskCreate(touch_pad_read_task, "touch_pad_read_task", 2048, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Touch pad started for ESP32-S3");
}

int uint_test() {
    // 注册回调函数

    // 启动触摸模块
    touch_pad_start();

    // 主循环
    while (1) {
        // 这里可以添加其他逻辑
        vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒延迟
    }

    return 0;
}

