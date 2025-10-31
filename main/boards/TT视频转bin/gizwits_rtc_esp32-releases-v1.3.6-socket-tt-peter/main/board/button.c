#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "board.h"

static const char *TAG = "BUTTON";
static button_callback_t button_cb = NULL;
static int button_gpio = -1;
static const button_config_t button_config = {
    .long_press_time = 2000,
    .double_click_time = 300
};

#define DEBOUNCE_TIME 30  // 消抖时间(ms)
// #define INPUT_KEY_NUM 4  // 最大支持的按钮数量
#define TIMER_TASK_STACK_SIZE 3072  // 增加定时器任务栈大小

typedef struct {
    int gpio_num;
    button_callback_t callback;
    button_config_t config;
    uint32_t press_start_time;
    uint32_t last_press_time;
    bool is_pressed;
    bool long_press_triggered;
    uint8_t click_count;
    TimerHandle_t double_click_timer;
    TimerHandle_t triple_click_timer;
    TimerHandle_t long_press_timer;
    TaskHandle_t task_handle;
} button_dev_t;

static button_dev_t *buttons[INPUT_KEY_NUM] = {0};

// 修改定时器回调，使其更简洁
static void long_press_timer_cb(TimerHandle_t timer) 
{
    button_dev_t *btn = (button_dev_t *)pvTimerGetTimerID(timer);
    if (!btn || !btn->callback) return;
    
    // 在任务中处理回调，而不是在定时器中直接处理
    if (btn->task_handle) {
        xTaskNotify(btn->task_handle, BUTTON_EVENT_LONG_PRESSED << 16, eSetValueWithOverwrite);
    }
}

static void double_click_timer_cb(TimerHandle_t timer) 
{
    button_dev_t *btn = (button_dev_t *)pvTimerGetTimerID(timer);
    if (!btn || !btn->callback) return;
    
    // 在任务中处理回调
    if (btn->task_handle) {
        xTaskNotify(btn->task_handle, BUTTON_EVENT_CLICKED << 16, eSetValueWithOverwrite);
    }
}

// 添加三击定时器回调
static void triple_click_timer_cb(TimerHandle_t timer) 
{
    button_dev_t *btn = (button_dev_t *)pvTimerGetTimerID(timer);
    if (!btn || !btn->callback) return;
    
    if (btn->click_count == 2) {  // 如果是两次点击
        if (btn->task_handle) {
            xTaskNotify(btn->task_handle, BUTTON_EVENT_DOUBLE_CLICKED << 16, eSetValueWithOverwrite);
        }
    } else if (btn->click_count == 1) {  // 如果是单次点击
        if (btn->task_handle) {
            xTaskNotify(btn->task_handle, BUTTON_EVENT_CLICKED << 16, eSetValueWithOverwrite);
        }
    }
    btn->click_count = 0;  // 重置点击计数
}

// 只保留关键的中断处理在 IRAM
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(arg, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void process_button_event(button_dev_t *btn, bool pressed) 
{
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    if (pressed) {
        btn->is_pressed = true;
        btn->press_start_time = current_time;
        btn->long_press_triggered = false;
        xTimerStart(btn->long_press_timer, 0);
        if (btn->callback) {
            btn->callback(BUTTON_EVENT_PRESSED);
        }
    } else {
        btn->is_pressed = false;
        xTimerStop(btn->long_press_timer, 0);
        
        if (!btn->long_press_triggered) {
            btn->click_count++;
            if (btn->click_count == 1) {
                // 启动三击定时器
                xTimerStart(btn->triple_click_timer, 0);
            } else if (btn->click_count == 2) {
                // 继续等待第三次点击
            } else if (btn->click_count == 3) {
                // 三击触发
                xTimerStop(btn->triple_click_timer, 0);
                btn->click_count = 0;
                if (btn->callback) {
                    btn->callback(BUTTON_EVENT_TRIPLE_CLICKED);
                }
            }
        }
        
        if (btn->callback) {
            btn->callback(BUTTON_EVENT_RELEASED);
        }
    }
}

static void button_task(void* arg)
{
    button_dev_t *btn = (button_dev_t *)arg;
    uint32_t notification;
    
    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, &notification, portMAX_DELAY) == pdTRUE) {
            // 检查是否是定时器事件
            if (notification & 0xFFFF0000) {
                button_event_t event = (notification >> 16);
                if (btn->callback) {
                    btn->callback(event);
                }
            } else {
                // 处理按钮状态变化
                vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));// todo 无法确保消抖时间内按键是否发送跳变
                bool pressed = !gpio_get_level(btn->gpio_num);
                process_button_event(btn, pressed);
            }
        }
    }
}

esp_err_t board_button_init(int gpio_num, const button_config_t *config, button_callback_t callback)
{
    // 查找空闲槽位
    int idx = -1;
    for (int i = 0; i < INPUT_KEY_NUM; i++) {
        if (buttons[i] == NULL) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return ESP_ERR_NO_MEM;

    // 分配按钮设备结构
    button_dev_t *btn = calloc(1, sizeof(button_dev_t));
    if (!btn) return ESP_ERR_NO_MEM;

    btn->gpio_num = gpio_num;
    btn->callback = callback;
    if (config) {
        btn->config = *config;
    } else {
        btn->config = button_config;
    }

    // 创建定时器时增加栈大小
    static StaticTimer_t timer_buffers[INPUT_KEY_NUM * 3];  // 为每个按钮的两个定时器预分配内存
    static int timer_index = 0;

    char tmr_name[16];
    snprintf(tmr_name, sizeof(tmr_name), "dbl_clk_%d", idx);
    btn->double_click_timer = xTimerCreateStatic(
        tmr_name, 
        pdMS_TO_TICKS(btn->config.double_click_time),
        pdFALSE, 
        btn,
        double_click_timer_cb,
        &timer_buffers[timer_index++]
    );

    snprintf(tmr_name, sizeof(tmr_name), "long_%d", idx);
    btn->long_press_timer = xTimerCreateStatic(
        tmr_name,
        pdMS_TO_TICKS(btn->config.long_press_time),
        pdFALSE,
        btn,
        long_press_timer_cb,
        &timer_buffers[timer_index++]
    );

    // 创建三击定时器
    snprintf(tmr_name, sizeof(tmr_name), "triple_%d", idx);
    btn->triple_click_timer = xTimerCreateStatic(
        tmr_name,
        pdMS_TO_TICKS(btn->config.double_click_time),  // 使用相同的时间间隔
        pdFALSE,
        btn,
        triple_click_timer_cb,
        &timer_buffers[timer_index++]
    );

    if (!btn->double_click_timer || !btn->long_press_timer || !btn->triple_click_timer) {
        free(btn);
        return ESP_ERR_NO_MEM;
    }

    // GPIO 配置
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << gpio_num),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        free(btn);
        return ret;
    }

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "btn_%d", idx);
    // 增大按键现场的优先级
    if (xTaskCreate(button_task, task_name, 1024 * 4, btn, 15, &btn->task_handle) != pdPASS) {
        free(btn);
        return ESP_ERR_NO_MEM;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        vTaskDelete(btn->task_handle);
        free(btn);
        return ret;
    }

    ret = gpio_isr_handler_add(gpio_num, gpio_isr_handler, btn->task_handle);
    if (ret != ESP_OK) {
        vTaskDelete(btn->task_handle);
        free(btn);
        return ret;
    }

    buttons[idx] = btn;
    ESP_LOGI(TAG, "Button %d initialized on GPIO %d", idx, gpio_num);
    return ESP_OK;
} 