# 产测串口复用修改总结

## 修改目标
将产测串口从独立的UART_NUM_1改为复用日志串口UART_NUM_0，通过错误日志发送数据，并接管RX接收。

## 主要修改内容

### 1. 串口配置修改
- **文件**: `main/factory_test/factory_test.cc`
- **修改**: 将 `FACTORY_TEST_UART_NUM` 从 `UART_NUM_1` 改为 `UART_NUM_0`
- **修改**: 使用 `CONFIG_ESP_CONSOLE_UART_TX_GPIO` 和 `CONFIG_ESP_CONSOLE_UART_RX_GPIO` 作为引脚配置

### 2. 数据发送方式修改
- **文件**: `main/factory_test/factory_test.cc`
- **修改**: `factory_test_send()` 函数中的 `uart_write_bytes()` 改为 `ESP_LOGE()`
- **新增**: 使用特殊前缀 `FT_DATA:` 标识产测数据
- **格式**: `ESP_LOGE(TAG, "FT_DATA: %.*s", len, data);`

### 3. 串口冲突处理
- **文件**: `main/factory_test/factory_test.cc`
- **新增**: UART驱动冲突检测和处理机制
- **新增**: `s_uart_taken_over` 标志避免重复初始化
- **新增**: 如果UART_NUM_0已被使用，会先卸载再重新安装

### 4. 调试信息增强
- **文件**: `main/factory_test/factory_test.cc`
- **新增**: 详细的初始化日志
- **新增**: 显示TX/RX引脚配置信息
- **新增**: 串口状态检查日志

### 5. 缺失函数实现
- **文件**: `main/factory_test/factory_test.cc`
- **新增**: `storage_save_factory_test_mode()` 函数实现
- **新增**: `storage_load_wifi_config()` 函数实现
- **新增**: `storage_save_wifi_config()` 函数实现
- **新增**: `storage_is_auth_valid()` 函数实现
- **新增**: `audio_tone_play()` 函数实现
- **新增**: `user_set_volume()` 函数实现
- **新增**: 版本信息定义 `HARD_VERSION` 和 `SOFT_VERSION`

### 6. 头文件包含
- **文件**: `main/factory_test/factory_test.cc`
- **新增**: `#include "freertos/FreeRTOS.h"`
- **新增**: `#include "freertos/task.h"`
- **新增**: `#include "settings.h"`

### 7. 测试文件
- **新增**: `main/factory_test/test_uart.cc` - 串口功能测试
- **新增**: `main/factory_test/UART_MODIFICATION.md` - 修改说明文档
- **新增**: `main/factory_test/CHANGES_SUMMARY.md` - 修改总结文档

### 8. 头文件更新
- **文件**: `main/factory_test/factory_test.h`
- **新增**: 测试函数声明 `test_factory_test_uart()` 和 `test_at_commands()`

## 技术细节

### 串口复用机制
1. **初始化检查**: 检查UART_NUM_0是否已被使用
2. **冲突处理**: 如果已被使用，先卸载再重新安装
3. **状态管理**: 使用 `s_uart_taken_over` 标志避免重复初始化

### 数据发送机制
1. **日志输出**: 通过 `ESP_LOGE()` 发送数据到串口
2. **前缀标识**: 使用 `FT_DATA:` 前缀便于识别和过滤
3. **格式保持**: 保持原有的数据格式和长度

### 接收处理机制
1. **任务创建**: 创建独立的串口接收任务
2. **缓冲区管理**: 使用动态分配的缓冲区
3. **命令解析**: 保持原有的AT命令解析逻辑

## 兼容性说明

### 与现有代码的兼容性
- ✅ 保持原有的AT命令接口
- ✅ 保持原有的产测模式管理
- ✅ 保持原有的IO测试功能
- ✅ 保持原有的录音播放功能

### 与其他组件的兼容性
- ⚠️ 与NFC功能可能存在冲突（都使用UART_NUM_0）
- ✅ 已添加冲突检测和处理机制
- ⚠️ 建议在产测模式下禁用其他UART_NUM_0用户

## 使用方法

### 启用产测模式
```bash
# 在NVS中设置
factory_test_mode = 1
# 重启设备
```

### 发送数据
- 产测数据会通过错误日志输出到串口
- 格式：`E (FT) FT_DATA: <数据内容>`

### 接收数据
- 串口RX由产测程序接管
- 支持AT命令格式：`AT+<命令>\r\n`

## 测试验证

### 编译测试
```bash
idf.py build
```

### 功能测试
```bash
# 设置产测模式
# 重启设备
# 通过串口发送AT命令测试
```

## 注意事项

1. **串口冲突**: 确保没有其他组件同时使用UART_NUM_0
2. **日志级别**: 确保错误日志级别已启用
3. **引脚配置**: 确认CONFIG_ESP_CONSOLE_UART_TX_GPIO和CONFIG_ESP_CONSOLE_UART_RX_GPIO配置正确
4. **内存使用**: 产测任务使用4KB栈空间

## 故障排除

### 串口无法初始化
- 检查是否有其他组件占用UART_NUM_0
- 查看日志中的错误信息
- 确认引脚配置正确

### 数据发送失败
- 检查日志级别设置
- 确认错误日志已启用
- 查看串口输出是否正常

### AT命令无响应
- 确认产测模式已启用
- 检查命令格式是否正确
- 查看任务是否正常启动 