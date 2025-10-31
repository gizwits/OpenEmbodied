#ifndef _RTC_HANDLER_H_
#define _RTC_HANDLER_H_

#include "esp_err.h"
#include "esp_dispatcher.h"
#include "esp_websocket_client.h"
#include "gizwits_protocol.h"
#include "gizwits_product.h"
#include "config/config.h"

bool get_wakeup_flag(void);
void set_wakeup_flag(bool flag);

#if defined(CONFIG_TT_MUSIC_HW_1V5)
#define SET_WAKEUP_FLAG(flag) do { \
    ESP_LOGI("wakeup", "Set wakeup flag at %s:%d to %s", __FILE__, __LINE__, flag ? "true" : "false"); \
    set_wakeup_flag(flag); \
    if (get_valuestate() != state_VALUE0_close) \
    {set_valuestate(flag?state_VALUE2_running:state_VALUE1_standby);}\
    if(flag)\
    {\
        set_last_audio_time(esp_timer_get_time());\
    }\
} while(0)
#else

#define SET_WAKEUP_FLAG(flag) do { \
    ESP_LOGI("wakeup", "Set wakeup flag at %s:%d to %s", __FILE__, __LINE__, flag ? "true" : "false"); \
    set_wakeup_flag(flag); \
    set_valuestate(flag?state_VALUE2_running:state_VALUE1_standby);\
    if(flag)\
    {\
        set_last_audio_time(esp_timer_get_time());\
    }\
} while(0)

#endif


void __set_voice_sleep_flag(const char *funN, uint32_t line, bool flag);
#define set_voice_sleep_flag(flag) __set_voice_sleep_flag(__func__, __LINE__, flag)
bool get_voice_sleep_flag(void);
bool get_is_playing_cache(void);
uint32_t get_last_audio_time(void);
void set_last_audio_time(uint32_t time);
void break_rec_with_key();
void send_wakeup_message(void);

bool get_user_speaking_flag(void);
void set_user_speaking_flag(bool flag);
esp_websocket_client_handle_t get_socket_client(void);


const static action_arg_t converted_sleep_action_arg = {
    .data = (void *)"spiffs://spiffs/converted_sleep.mp3"
};
const static action_result_t converted_sleep_result = {0};

const static action_arg_t converted_turn_on_action_arg = {
    .data = (void *)"spiffs://spiffs/converted_turn_on.mp3"
 };
const static action_result_t converted_turn_on_result = {0};


// 加入RTC房间
void join_room(rtc_params_t* params, bool is_switch);
// 确保函数声明存在
void leave_room(void);

void start_recorder_with_key(void);

void start_recorder(void);
void end_recorder(void);
void end_recorder_task(void);
// 添加到头文件中
bool is_websocket_connected(void);
void set_websocket_connection_callback(void (*callback)(bool connected));

bool should_discard_first_i2s_data(void);

bool get_is_playing_cache(void);
// 录音任务计数器相关函数
void increment_recorder_counter(void);
void decrement_recorder_counter(void);
int get_recorder_counter(void);
void reset_recorder_counter(void);
void send_hello_message(void);

// 添加函数声明
void play_network_error_with_debounce(void);

void cancel_ai_agent_task(void * para);

esp_err_t g_recorder_pipeline_resume();
esp_err_t g_recorder_pipeline_pause();
bool g_audio_recorder_get_wakeup_state();

#endif
