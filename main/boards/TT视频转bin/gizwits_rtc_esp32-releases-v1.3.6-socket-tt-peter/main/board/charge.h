#ifndef _CHARGE_H_
#define _CHARGE_H_

#include <stdint.h>
#include "esp_err.h"

// 充电状态枚举
typedef enum {
    BATTERY_NOT_CHARGING,  // 未充电
    BATTERY_CHARGING,      // 正在充电
    BATTERY_FULL          // 电池已充满
} battery_state_t;

// 初始化充电检测
void charge_init(void);

battery_state_t check_battery_state(void);
// 获取当前充电状态
battery_state_t get_battery_state(void);
const char* get_battery_state_str(void);

void battery_check_cb();
void charge_init_no_task(void);
#endif 