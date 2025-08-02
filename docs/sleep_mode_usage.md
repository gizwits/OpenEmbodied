# 休眠模式使用说明

## 概述

设备现在支持休眠模式，可以关闭屏幕、socket 连接和 wifi 连接以节省电量。当从休眠模式切换到其他状态时，系统会自动重新连接 wifi。

## 新增状态

在 `DeviceState` 枚举中添加了新的状态：
- `kDeviceStateSleeping`: 休眠模式

## 新增方法

### Application 类新增方法

1. **EnterSleepMode()**
   - 功能：进入休眠模式
   - 效果：关闭屏幕背光、socket 连接和 wifi 连接

2. **ExitSleepMode()**
   - 功能：退出休眠模式
   - 效果：重新连接 wifi、恢复屏幕背光，并切换到空闲状态

## 状态切换逻辑

### 进入休眠模式
当调用 `SetDeviceState(kDeviceStateSleeping)` 时：
1. 关闭屏幕背光（设置为 0）
2. 关闭 socket 连接（调用 `protocol_->CloseAudioChannel()`）
3. 关闭 wifi 连接（调用 `wifi_station.Stop()`）
4. 设置显示状态为待机，表情为 sleepy

### 从休眠模式切换到其他状态
当从 `kDeviceStateSleeping` 切换到其他状态时：
1. 自动重新连接 wifi（调用 `wifi_station.Start()` 和 `wifi_station.WaitForConnected()`）
2. 恢复屏幕背光（调用 `backlight->RestoreBrightness()`）
3. 如果 wifi 连接失败，会保持在休眠状态

## LED 状态

在休眠模式下，LED 会显示最低亮度：
- GPIO LED: 亮度设置为 1
- 环形 LED: RGB 值都设置为 1
- 单 LED: RGB 值都设置为 1

## 使用示例

### 基本使用

```cpp
// 进入休眠模式
Application::GetInstance().EnterSleepMode();

// 退出休眠模式
Application::GetInstance().ExitSleepMode();

// 直接设置状态（会自动处理 wifi 重连）
Application::GetInstance().SetDeviceState(kDeviceStateIdle);
```

### 按钮处理示例

```cpp
void InitializeButtons() {
    boot_button_.OnClick([this]() {
        auto& app = Application::GetInstance();
        auto current_state = app.GetDeviceState();
        
        // 如果当前在休眠状态，退出休眠
        if (current_state == kDeviceStateSleeping) {
            app.ExitSleepMode();
            return;
        }
        
        // 如果当前在空闲状态，进入休眠
        if (current_state == kDeviceStateIdle) {
            app.EnterSleepMode();
            return;
        }
        
        // 其他状态的处理...
        app.ToggleChatState();
    });
    
    // 长按进入休眠模式
    boot_button_.OnLongPress([this]() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateSleeping) {
            app.EnterSleepMode();
        }
    });
}
```

## 注意事项

1. 从休眠模式切换到其他状态时，系统会等待最多 30 秒来重新连接 wifi
2. 如果 wifi 连接失败，设备会保持在休眠状态
3. 休眠模式下，所有网络功能都会被禁用
4. LED 在休眠模式下会显示最低亮度而不是完全关闭，以便用户知道设备仍在工作

## 兼容性

这个功能与现有的电源管理功能兼容，可以与其他省电模式结合使用。 