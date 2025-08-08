# 产测串口复用修改说明

## 修改概述

本次修改将产测串口从独立的UART_NUM_1改为复用日志串口UART_NUM_0，通过错误日志发送数据。

## 主要修改内容

### 1. 串口配置修改
- 将 `FACTORY_TEST_UART_NUM` 从 `UART_NUM_1` 改为 `UART_NUM_0`
- 使用 `CONFIG_ESP_CONSOLE_UART_TX_GPIO` 和 `CONFIG_ESP_CONSOLE_UART_RX_GPIO` 作为引脚配置

### 2. 数据发送方式修改
- 原来的 `uart_write_bytes()` 改为通过 `ESP_LOGE()` 发送
- 使用特殊前缀 `FT_DATA:` 标识产测数据
- 数据格式：`ESP_LOGE(TAG, "FT_DATA: %.*s", len, data);`

### 3. 串口冲突处理
- 添加了UART驱动冲突检测和处理
- 如果UART_NUM_0已被使用，会先卸载再重新安装
- 添加了 `s_uart_taken_over` 标志避免重复初始化

### 4. 调试信息增强
- 添加了详细的初始化日志
- 显示TX/RX引脚配置信息
- 添加了串口状态检查日志

## 使用方法

### 启用产测模式
1. 设置 `factory_test_mode` 为 1
2. 重启设备
3. 产测会自动接管串口并开始监听AT命令

### 发送数据
- 产测数据会通过错误日志输出到串口
- 格式：`E (FT) FT_DATA: <数据内容>`

### 接收数据
- 串口RX由产测程序接管
- 支持AT命令格式：`AT+<命令>\r\n`

## 兼容性说明

### 与其他组件的兼容性
- 与NFC功能可能存在冲突（都使用UART_NUM_0）
- 已添加冲突检测和处理机制
- 建议在产测模式下禁用其他UART_NUM_0用户

### 日志输出
- 产测数据会混在正常日志中
- 使用 `FT_DATA:` 前缀便于识别和过滤
- 可以通过日志级别控制输出

## 测试方法

### 编译测试
```bash
# 启用产测模式
idf.py menuconfig
# 在 Component config -> Factory Test Mode 中启用
```

### 功能测试
```bash
# 设置产测模式
# 在NVS中设置 factory_test_mode = 1
# 重启设备
# 通过串口发送AT命令测试
```

## 注意事项

1. **串口冲突**：确保没有其他组件同时使用UART_NUM_0
2. **日志级别**：确保错误日志级别已启用
3. **引脚配置**：确认CONFIG_ESP_CONSOLE_UART_TX_GPIO和CONFIG_ESP_CONSOLE_UART_RX_GPIO配置正确
4. **内存使用**：产测任务使用4KB栈空间

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