#ifndef __TEST_STREAM_H__
#define __TEST_STREAM_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    int seconds;
    int ch_idx;
} ft_record_arg_t;

typedef struct {
    int seconds;
    void *buffer;
    int size;
} ft_play_arg_t;

typedef enum {
    CHANNEL_MIC = 0,
    CHANNEL_AEC,
} record_channel_t;

void ft_record_task(void *arg);
int ft_start_record_task(int channel, int seconds);
int ft_start_play_task(int seconds, void *buffer, int size);

char *ft_get_record_buffer(void);
int ft_get_record_buffer_index(void);

#ifdef __cplusplus
}
#endif

#endif /* __FACTORY_TEST_H__ */
