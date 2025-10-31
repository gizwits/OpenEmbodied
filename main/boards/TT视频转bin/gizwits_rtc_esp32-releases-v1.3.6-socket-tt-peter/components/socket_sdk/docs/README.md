# SDK API 使用文档

以下是对 SDK API 的分析和使用说明，帮助开发者了解和使用这些接口。

[v0.0.1](https://goms-1251025085.cos.ap-guangzhou.myqcloud.com/cdn/gizwits-coze-socket-s3.zip)


## 目录

1. [概述](#概述)
2. [初始化](#初始化)
3. [版本信息](#版本信息)
4. [设备控制](#设备控制)
5. [录音管理](#录音管理)
6. [事件通知](#事件通知)
7. [回调函数](#回调函数)

## 概述

SDK API 提供了一组函数和枚举，用于控制和管理设备的各种功能，包括初始化、版本信息获取、WiFi 重置、电源控制、录音管理和事件通知等。

## 初始化

```c
void sdk_init(const char* hard_version, const char* soft_version, activate_mode_t activate_mode);
```

**功能**：初始化 SDK，设置硬件版本、软件版本和激活模式。

**参数**：
- `hard_version`：硬件版本号字符串
- `soft_version`：软件版本号字符串
- `activate_mode`：设备激活模式，可选值如下：
  - `ACTIVED_MODE_SERVER_VAD`：通过服务器端 VAD (Voice Activity Detection) 激活
  - `ACTIVED_MODE_BUTTON`：通过按钮按压激活
  - `ACTIVED_MODE_BUTTON_AND_WAKEUP`：通过按钮按压或唤醒词激活
  - `ACTIVED_MODE_SERVER_VAD_AND_WAKEUP`：通过服务器端 VAD 或唤醒词激活

**示例**：
```c
sdk_init("ATOM002", "WS010407", ACTIVED_MODE_BUTTON_AND_WAKEUP);
```

## 版本信息

```c
const char* sdk_get_hardware_version(void);
const char* sdk_get_software_version(void);
```

**功能**：获取设备的硬件和软件版本信息。

**返回值**：版本号字符串。

**示例**：
```c
const char* hw_version = sdk_get_hardware_version();
const char* sw_version = sdk_get_software_version();
printf("Hardware Version: %s\n", hw_version);
printf("Software Version: %s\n", sw_version);
```

## 设备控制

### WiFi 重置

```c
void sdk_wifi_reset(void);
```

**功能**：重置模组，清除 WiFi 信息和解除绑定关系。调用结束后会触发模组重启进入待配网状态。

**示例**：
```c
sdk_wifi_reset();
```

## 录音管理

```c
void sdk_start_record(void);
void sdk_stop_record(void);
void sdk_break_record(void);
```

**功能**：
- `sdk_start_record`：开始录音
- `sdk_stop_record`：正常结束录音
- `sdk_break_record`：中断录音

**示例**：
```c
// 开始录音
sdk_start_record();

// 正常结束录音
sdk_stop_record();

// 或者在需要中断时
sdk_break_record();
```

## 事件通知

SDK 定义了一系列用户事件，用于通知各种状态变化：

```c
typedef enum {
    // 适配按键事件
    USER_EVENT_SLEEP,               // 休眠事件
    USER_EVENT_SET_VOLUME,          // 设置音量
    // 适配LED事件
    USER_EVENT_WAKEUP,              // 唤醒事件
    USER_EVENT_CHANGING_AI_AGENT,   // 切换AI
    USER_EVENT_CHAT_IN_PROGRESS,    // coze 受理中
    USER_EVENT_WIFI_INIT,           // 进入配网状态
    USER_EVENT_WIFI_CONNECTED,      // wifi 连接成功
    USER_EVENT_WIFI_RECONNECTED,    // wifi 重新连接
    USER_EVENT_WIFI_RECONNECT_FAILED, // wifi 重新连接失败
    USER_EVENT_WIFI_CONNECTING,     // wifi 连接中
    USER_EVENT_WIFI_DISCONNECTED,   // wifi 断开连接
    USER_EVENT_USER_SPEAKING,       // 用户说话（RGB幻彩闪烁）
    USER_EVENT_AI_SPEAKING,         // AI说话（RGB幻彩呼吸）
    USER_EVENT_STANDBY,             // 待机状态
    USER_EVENT_NET_WORK_ERROR,      // 网络错误
} user_event_t;
```

## 回调函数

SDK 提供了两种回调函数类型，用于处理不同类型的事件：

```c
/* Callback function types */
typedef void (*coze_plugin_notify_cb)(char *data);
typedef void (*user_event_notify_cb)(user_event_t event, cJSON *json_data);

/* Set callback functions */
void sdk_set_coze_plugin_notify_callback(coze_plugin_notify_cb cb);
void sdk_set_user_event_notify_callback(user_event_notify_cb cb);
```

**功能**：
- `sdk_set_coze_plugin_notify_callback`：设置 Coze 插件通知回调函数
- `sdk_set_user_event_notify_callback`：设置用户事件通知回调函数

**示例**：
```c
// 用户事件回调示例
void handle_user_event(user_event_t event, cJSON *json_data) {
    switch(event) {
        case USER_EVENT_WAKEUP:
            printf("设备被唤醒\n");
            break;
        case USER_EVENT_WIFI_CONNECTED:
            printf("WiFi已连接\n");
            break;
        // 处理其他事件...
    }
}

// Coze插件通知回调示例
void handle_coze_plugin_notify(char *data) {
    printf("收到Coze插件通知: %s\n", data);
    // 处理插件数据...
}

// 设置回调函数
sdk_set_user_event_notify_callback(handle_user_event);
sdk_set_coze_plugin_notify_callback(handle_coze_plugin_notify);
```

## 配置参数

SDK 定义了一些配置参数：

```c
/* 休眠 */
#define CONFIG_SLEEP_TIMEOUT_SEC 120
#define SLEEP_TIME CONFIG_SLEEP_TIMEOUT_SEC * 1000 * 1000
```

- `CONFIG_SLEEP_TIMEOUT_SEC`：休眠超时时间（秒）
- `SLEEP_TIME`：休眠时间（微秒）

以上是SDK API的主要功能和用法。根据具体应用场景，调用相应的API来实现设备控制和事件处理。
