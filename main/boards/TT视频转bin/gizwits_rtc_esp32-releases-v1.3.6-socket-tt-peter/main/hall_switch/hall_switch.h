
#ifndef __HALL_SWITCH_H__
#define __HALL_SWITCH_H__

#define HALL_SENSOR_GPIO GPIO_NUM_7  // 假设霍尔传感器连接在GPIO4

typedef enum {
    HALL_STATE_OFF = 0,
    HALL_STATE_ON = 1
} hall_state_t;

void hall_sensor_init();
void hall_timer_callback();
uint8_t __get_hall_state(const char* fun, int32_t line);
#define get_hall_state() __get_hall_state(__func__, __LINE__)
uint8_t get_hall_open_once(void);
#endif

