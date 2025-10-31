#ifndef __MQTT_PROTOCOL_H__
#define __MQTT_PROTOCOL_H__
#include <stdint.h>


int8_t mqtt_len_of_bytes(int32_t len, uint8_t *buf);
uint8_t mqtt_num_rem_len_bytes(const uint8_t *buf);
#endif

