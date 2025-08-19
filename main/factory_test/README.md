# 产测模块 C++ 转换说明

## 概述

本项目将原有的C语言产测模块转换为C++实现，以更好地适配项目的整体架构。

## 转换文件列表

### 头文件转换
- `factory_test.h` → `factory_test.hh`
- `test_stream.h` → `test_stream.hh`

### 源文件转换
- `factory_test.c` → `factory_test.cc`
- `test_stream.c` → `test_stream.cc`

## 主要改进

### 1. 使用现代C++特性
- 使用 `std::vector` 替代原始数组
- 使用 `std::chrono` 进行时间管理
- 使用 `static_cast` 和 `reinterpret_cast` 进行类型转换
- 使用 `nullptr` 替代 `NULL`

### 2. 音频处理架构适配
- 使用项目的 `AudioCodec` 类进行音频处理
- 集成 `Board::GetInstance()` 获取音频编解码器
- 使用 `std::vector<int16_t>` 进行音频数据管理

### 3. 内存管理改进
- 使用 RAII 原则进行资源管理
- 使用 `std::vector` 自动内存管理
- 避免手动内存分配和释放

## 功能特性

### 录音功能
- `ft_start_record_task(int seconds)` - 启动录音任务
- `ft_stop_record_task()` - 停止录音任务
- `ft_is_recording()` - 检查录音状态
- `ft_get_recorded_samples()` - 获取录音样本数

### 播放功能
- `ft_start_play_task(int seconds)` - 启动播放任务
- `ft_stop_play_task()` - 停止播放任务
- `ft_is_playing()` - 检查播放状态

### 缓冲区管理
- `ft_init_record_buffer(int seconds)` - 初始化录音缓冲区
- `ft_clear_record_buffer()` - 清理录音缓冲区

### 产测功能
- `factory_test_init()` - 初始化产测
- `factory_test_is_enabled()` - 检查产测是否启用
- `factory_test_is_aging()` - 检查是否处于老化测试模式
- `factory_start_aging_task()` - 启动老化测试任务

## 使用示例

```cpp
#include "factory_test.hh"
#include "test_stream.hh"

// 检查产测模式
if (factory_test_is_enabled()) {
    // 启动录音 (5秒)
    if (ft_start_record_task(5) == 0) {
        // 等待录音完成
        while (ft_is_recording()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 播放录音 (5秒)
        ft_start_play_task(5);
        
        // 等待播放完成
        while (ft_is_playing()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 清理缓冲区
        ft_clear_record_buffer();
    }
}
```

## 兼容性说明

- 保持了原有的AT命令接口
- 保持了原有的产测模式管理
- 保持了原有的IO测试功能
- 音频处理方式适配了项目的 `AudioCodec` 架构

## 编译要求

- 需要支持C++11或更高版本
- 需要包含项目的 `board.h` 和 `application.h` 头文件
- 需要链接项目的音频编解码器库

## 注意事项

1. 新的实现使用了项目的音频编解码器架构，确保在编译时正确链接相关库
2. 录音和播放功能现在使用 `std::vector` 进行数据管理，内存使用更加安全
3. 时间管理使用 `std::chrono`，提供更精确的时间控制
4. 所有函数都添加了适当的错误检查和日志输出 