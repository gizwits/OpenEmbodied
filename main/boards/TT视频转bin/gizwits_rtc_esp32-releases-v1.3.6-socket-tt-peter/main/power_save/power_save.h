#ifndef __POWER_SAVE_H__
#define __POWER_SAVE_H__
#include "esp_sleep.h"
#include "esp_log.h"
#include "hall_switch.h"
#include "driver/gpio.h"

void deep_sleep_enter(void);
void wakeup_cause_print(void);


#endif
