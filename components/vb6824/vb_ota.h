#ifndef _JL_OTA_H_
#define _JL_OTA_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"

typedef enum{
    JL_OTA_START,
    JL_OTA_STOP,
    JL_OTA_FAIL,
    JL_OTA_PROCESS,
    JL_OTA_SUCCESS,
    JL_OTA_RETRY,
    JL_OTA_REGET_WAKE,
}jl_ota_evt_id;

typedef void (*jl_ota_event_t)(jl_ota_evt_id evt, uint32_t data);

void jl_ondata(uint8_t *buf, uint16_t len);
int jl_set_ota_url(const char* url);
int jl_ota_start(jl_ota_event_t evt_cb);
int jl_ota_set_code(char  *code);
void jl_set_uart_port(uint8_t port);


#ifdef __cplusplus
}
#endif
#endif