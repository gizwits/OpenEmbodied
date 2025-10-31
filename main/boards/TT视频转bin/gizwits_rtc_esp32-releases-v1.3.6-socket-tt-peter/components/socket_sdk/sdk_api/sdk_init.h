#ifndef _SDK_INIT_H_
#define _SDK_INIT_H_


#include "audio_hal.h"
#include "storage.h"
#include "board.h"

extern int sdk_volume;
extern audio_board_handle_t audio_board_handle;

// #define NO_SLEEP

/**
 * @brief Generate a random trace ID (UUID v4 format)
 * 
 * @param trace_id Buffer to store the generated trace ID
 * @param size Size of the buffer (should be at least 37 bytes for full UUID)
 * @return ESP_OK if successful, otherwise an error code
 */
void gen_trace_id();
char* get_trace_id();

#endif /* _SDK_INIT_H_ */ 
