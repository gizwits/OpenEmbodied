#ifndef __VB6824_H__
#define __VB6824_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "driver/gpio.h"

typedef enum {
    VB6824_EVT_OTA_ENTER = 0,
    VB6824_EVT_OTA_START = 2,
    VB6824_EVT_OTA_EXIT = 3,
    VB6824_EVT_OTA_PROGRESS = 4,
    VB6824_EVT_OTA_SUCCESS = 5,
    VB6824_EVT_OTA_FAIL = 6,
} vb6824_evt_t;

typedef void (*vb_voice_command_cb_t)(char *command, uint16_t len, void *arg);
typedef void (*vb_voice_event_cb_t)(vb6824_evt_t event_id, uint32_t data, void *arg);

void jl_ws_stop();
int jl_ws_is_start();
int jl_ws_start(char *code);
bool vb6824_is_support_ota();

void vb6824_register_voice_command_cb(vb_voice_command_cb_t cb, void *arg);
void vb6824_register_event_cb(vb_voice_event_cb_t cb, void *arg);

void vb6824_audio_enable_input(bool enable);
void vb6824_audio_enable_output(bool enable);

void vb6824_audio_set_output_volume(uint8_t volume);

void vb6824_audio_write(uint8_t *data, uint16_t len);
uint16_t vb6824_audio_read(uint8_t *data, uint16_t size);

void vb6824_init(gpio_num_t tx, gpio_num_t rx);

char *vb6824_get_wakeup_word();

#ifdef __cplusplus
}
#endif

#endif //__VB6824_H__