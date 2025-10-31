#ifndef _BOARD_INIT_H_
#define _BOARD_INIT_H_

#include "esp_err.h"
#include "driver/touch_pad.h"
#include "soc/touch_sensor_periph.h"
#include "sdk_api.h"

// 定义按键事件类型
typedef enum {
    KEY_EVENT_SLEEP,      // 休眠事件
    KEY_EVENT_RESET,      // 重置事件
    KEY_EVENT_REC_START,  // 录音开始事件
    KEY_EVENT_REC_BREAK,  // 录音中断事件   // MARK url play打断上报
    KEY_EVENT_REC_CLEAN,  // 录音中断事件
    KEY_EVENT_REC_STOP,   // 录音停止事件
    KEY_EVENT_VOLUME_UP,  // 音量增加
    KEY_EVENT_VOLUME_DOWN,// 音量减少
    KEY_EVENT_POWER_OFF,  // 关机
    KEY_RUN_WAKE_UP,      // 唤醒
    KEY_RUN_WAKE_UP_BY_CAP, // 盖子唤醒
    KEY_EVENT_TEST_LED,   // 测试所有灯效
    KEY_EVENT_AUDIO_LOG,  // 开关音频日志
    KEY_EVENT_ONBOARDING, // 非重置进入配网
} key_event_t;


/* 音量 */
#define DEFAULT_LOGIC_VOLUME    90
#define LOGIC_MAX_VOLUME        100
#define LOGIC_MIN_VOLUME        30

#define REAL_MAX_VOLUME         100

// 函数声明
void init_board();

void user_set_volume_add(int vol);
void user_set_volume_sub(int vol);
void user_set_volume(int vol);
int user_get_volume();
void user_set_volume_no_nvs();
void user_volume_set_tone_play();
void set_audio_mute(bool mute);
#endif /* _BOARD_INIT_H_ */
