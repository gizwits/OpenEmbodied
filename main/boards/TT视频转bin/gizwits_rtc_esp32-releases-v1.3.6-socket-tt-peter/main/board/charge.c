#include "board/charge.h"
#include "board/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdk_api.h"
#include "factory_test/factory_test.h"
#include "battery.h"
#include "gizwits_protocol.h"

#if defined CONFIG_AUDIO_BOARD_ATOM_V1 || defined CONFIG_AUDIO_BOARD_ATOM_V1_2
#define CHARGING_PIN     11   // CHRG pin
#define STANDBY_PIN      3    // STDBY pin
#elif defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
#define CHARGING_PIN     18   // CHRG pin
#define STANDBY_PIN      3    // STDBY pin
#elif defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
#define CHARGING_PIN     11   // CHRG pin
#define STANDBY_PIN      12   // STDBY pin
#endif


static const char *TAG = "CHARGE";
static battery_state_t current_battery_state = BATTERY_NOT_CHARGING;

// 为静态任务分配堆栈和控制块
static StackType_t *battery_check_task_stack = NULL;
static StaticTask_t battery_check_task_buffer;
static TaskHandle_t battery_check_task_handle = NULL;

// 初始化任务堆栈
static bool init_battery_check_task_stack(void) {
    battery_check_task_stack = (StackType_t *)heap_caps_malloc(1024 * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!battery_check_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate battery check task stack");
        return false;
    }
    return true;
}

// 清理任务堆栈
static void cleanup_battery_check_task_stack(void) {
    if (battery_check_task_stack) {
        heap_caps_free(battery_check_task_stack);
        battery_check_task_stack = NULL;
    }
}

// 检查充电状态
battery_state_t check_battery_state(void) {

    if(battery_get_voltage()>BATTERY_FULL_VOLTAGE) {
        return BATTERY_FULL;
    }

    // int chrg = gpio_get_level(CHARGING_PIN);
    // int stdby = gpio_get_level(STANDBY_PIN);
    
    // if (chrg == 0) {  // CHRG为低电平表示正在充电
    //     return BATTERY_CHARGING;
    // } else if (stdby == 0) {  // STDBY为低电平表示充满
    //     return BATTERY_FULL;
    // }
    return BATTERY_NOT_CHARGING;
}

// 充电状态检测任务
static void battery_check_task(void *arg) {
    // 初始化 GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << CHARGING_PIN) | (1ULL << STANDBY_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int8_t last_state = -1;
    
    while (1) {
        current_battery_state = check_battery_state();
        
        if (current_battery_state != last_state) {
            ESP_LOGI(TAG, "Battery state changed: %d -> %d", last_state, current_battery_state);
            
            if (!factory_test_is_aging()) {
                // 状态变化时更新LED，跟老化测试的灯状态冲突
                switch (current_battery_state) {
                case BATTERY_CHARGING:
                    led_set_state(LED_STATE_CHARGING);
                    set_valuecharge_status(charge_status_VALUE1_charging);
                    break;
                case BATTERY_FULL:
                    led_set_state(LED_STATE_FULL_BATTERY);
                    set_valuecharge_status(charge_status_VALUE2_charge_done);
                    break;
                case BATTERY_NOT_CHARGING:
                    set_valuecharge_status(charge_status_VALUE0_none);
                    // led_task_stop();
                    break;
                }
            }
            last_state = current_battery_state;
            system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void charge_init(void) {
    // 初始化任务堆栈
    if (!init_battery_check_task_stack()) {
        ESP_LOGE(TAG, "Failed to initialize battery check task stack");
        return;
    }

    // 创建充电检测任务
    battery_check_task_handle = xTaskCreateStaticPinnedToCore(
        battery_check_task,
        "battery_check",
        1024 * 4,
        NULL,
        2,
        battery_check_task_stack,
        &battery_check_task_buffer,
        0
    );
    if (battery_check_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create battery check task");
        cleanup_battery_check_task_stack();
    }
}

// 获取当前充电状态
battery_state_t get_battery_state(void) {
    return current_battery_state;
}

// 获取当前充电状态
const char* get_battery_state_str(void) {
    switch (current_battery_state) {
        case BATTERY_CHARGING:
            return "charging";
        case BATTERY_FULL:
            return "full";
        case BATTERY_NOT_CHARGING:
            return "discharging";
        default:
            return "unknown";
    }
}



// 充电状态检测任务
void battery_check_cb() {

    static int8_t last_state = -1;
    
    current_battery_state = check_battery_state();
    
    if (current_battery_state != last_state) {
        ESP_LOGI(TAG, "Battery state changed: %d -> %d", last_state, current_battery_state);
        
        if (!factory_test_is_aging()) {
            // 状态变化时更新LED，跟老化测试的灯状态冲突
            switch (current_battery_state) {
            case BATTERY_CHARGING:
                led_set_state(LED_STATE_CHARGING);
                set_valuecharge_status(charge_status_VALUE1_charging);
                break;
            case BATTERY_FULL:
                led_set_state(LED_STATE_FULL_BATTERY);
                set_valuecharge_status(charge_status_VALUE2_charge_done);
                break;
            case BATTERY_NOT_CHARGING:
                set_valuecharge_status(charge_status_VALUE0_none);
                // led_task_stop();
                break;
            }
        }
        last_state = current_battery_state;
        system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    }
    
}

void charge_init_no_task(void) {
    // 初始化 GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << CHARGING_PIN) | (1ULL << STANDBY_PIN),
        .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
}
