#ifndef __SDK_API_H__
#define __SDK_API_H__

#include "cJSON.h"
/* 用于提供 SDK开发API列表 */

/* 配置 */
// 版本号
// #define SOFT_VERSION "WS010407" // todo 目前该参数无效 后续把lib里使用的版本号改成入参
// #define HARD_VERSION "ATOM002"  // todo 目前该参数无效 后续把lib里使用的版本号改成入参

/* 休眠 */
#define CONFIG_SLEEP_TIMEOUT_SEC 3 *10    // todo Peter mark
#define SLEEP_TIME CONFIG_SLEEP_TIMEOUT_SEC * 1000 * 1000

// 添加一个新的枚举类型定义
typedef enum {
    SDK_ACTIVED_MODE_SERVER_VAD = 0,
    SDK_ACTIVED_MODE_BUTTON,
    SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP,
    SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP,
} sdk_actived_mode_t;

/* 初始化 */
void sdk_init(const char* hard_version, const char* soft_version, sdk_actived_mode_t actived_mode);

/* 获取版本信息 */
const char* sdk_get_hardware_version(void);
const char* sdk_get_software_version(void);

/* 重置模组 （清除wifi信息和解除绑定关系) */
/* 调用结束后会触发模组重启进入待配网状态 */
void sdk_wifi_reset( void );
/* 录音开始 */
void sdk_start_record(void);

/* 获取当前激活模式 */
sdk_actived_mode_t get_current_actived_mode(void);
/* 录音结束 */
void sdk_stop_record(void);
/* 录音中断 */
void sdk_break_record(void);

void run_sleep_task();
/* 用户事件通知处理 */
// 定义按键事件类型
typedef enum {
    // 适配按键事件
    USER_EVENT_SLEEP,      // 休眠事件
    USER_EVENT_SET_VOLUME,  // 设置音量
    // 适配LED事件
    USER_EVENT_WAKEUP, // 唤醒事件
    USER_EVENT_CHANGING_AI_AGENT, // 切换ai
    USER_EVENT_CHAT_IN_PROGRESS, // coze 受理中
    USER_EVENT_WIFI_INIT, // 进入配网状态
    USER_EVENT_WIFI_CONNECTED, // wifi 连接成功
    USER_EVENT_WIFI_RECONNECTED, // wifi 重新连接
    USER_EVENT_WIFI_RECONNECT_FAILED, // wifi 重新连接失败
    USER_EVENT_WIFI_CONNECTING, // wifi 连接中
    USER_EVENT_WIFI_DISCONNECTED, // wifi 断开连接
    USER_EVENT_USER_SPEAKING,     // 用户说话（RGB幻彩闪烁）    // MARK url play打断上报
    USER_EVENT_AI_SPEAKING,       // AI说话（RGB幻彩呼吸）      // MARK url play打断上报
    USER_EVENT_STANDBY, // 待机状态
    USER_EVENT_NET_WORK_ERROR,  // 网络错误
} user_event_t;

/* Callback function types */
typedef void (*coze_plugin_notify_cb)(char *data);
typedef void (*user_event_notify_cb)(user_event_t event, cJSON *json_data);

/* Set callback functions */
void sdk_set_coze_plugin_notify_callback(coze_plugin_notify_cb cb);
void sdk_set_user_event_notify_callback(user_event_notify_cb cb);
#endif