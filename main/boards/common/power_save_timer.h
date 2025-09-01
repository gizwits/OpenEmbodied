#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

/**
 * PowerSaveTimer - 电源管理定时器
 * 
 * 支持两种模式：
 * 1. 传统模式：使用独立的 esp_timer 任务，占用约 2-3KB 内存
 * 2. 轻量级模式：集成到 main loop，仅占用约 50 字节内存
 * 
 * 使用方法：
 * 1. 创建实例时默认启用轻量级模式
 * 2. 在 Application::MainEventLoop() 中自动调用 Update()
 * 3. 如需使用传统模式，调用 SetLightweightMode(false)
 * 
 * 内存占用对比：
 * - 传统模式：~2-3KB (esp_timer 任务栈 + 定时器句柄 + 回调对象)
 * - 轻量级模式：~50 字节 (类成员变量 + 时间戳)
 * 
 * 性能影响：
 * - 轻量级模式：每次 main loop 检查时间戳，开销极小
 * - 传统模式：独立任务，无额外开销
 */
class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    void WakeUp();
    void ResetTimer();  // 新增：重置计数器但不改变睡眠状态
    bool IsInSleepMode() { return in_sleep_mode_; }
    
    // 新增：轻量级模式，集成到 main loop
    void SetLightweightMode(bool enabled) { lightweight_mode_ = enabled; }
    void Update();  // 新增：在 main loop 中调用此方法

private:
    void PowerSaveCheck();
    void InitializeTimer();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool lightweight_mode_ = false;  // 新增：轻量级模式标志
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    
    // 轻量级模式相关
    uint64_t last_update_time_ = 0;
    static constexpr uint64_t UPDATE_INTERVAL_US = 1000000;  // 1秒更新间隔

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
