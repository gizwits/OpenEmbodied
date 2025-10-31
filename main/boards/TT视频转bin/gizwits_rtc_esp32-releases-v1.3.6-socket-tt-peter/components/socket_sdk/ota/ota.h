#ifndef _OTA_H_
#define _OTA_H_

void run_start_ota_task(char *sw_ver, char *url);

typedef struct {
    char *sw_ver;
    char *url;
} ota_task_params_t;

#endif