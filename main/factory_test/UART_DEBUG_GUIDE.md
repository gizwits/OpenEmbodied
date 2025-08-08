# 产测串口调试指南

## 问题诊断步骤

### 1. 检查编译错误
```bash
idf.py build
```
确保没有编译错误。

### 2. 检查串口配置
启动后查看日志，确认：
- 串口初始化是否成功
- TX/RX引脚配置是否正确
- 波特率是否为115200

### 3. 检查串口状态
在产测启动时，会输出以下信息：
```
I (FT) Initializing factory test UART on UART_NUM_0
I (FT) TX Pin: 1, RX Pin: 3
I (FT) Testing UART communication...
E (FT) FT_DATA: UART_TEST: Hello World
I (FT) UART config - baud_rate: 115200, data_bits: 3, parity: 0, stop_bits: 1
I (FT) UART pins - TX: 1, RX: 3, RTS: -1, CTS: -1
```

### 4. 检查数据接收
启动后，串口任务会每100次循环输出一次状态：
```
I (FT) UART read loop, counter: 100, last read len: 0
```

## 常见问题及解决方案

### 问题1: 编译错误 - CONFIG_ESP_CONSOLE_UART_TX_GPIO 未定义
**解决方案**: 已添加默认引脚配置，使用 GPIO_NUM_1 (TX) 和 GPIO_NUM_3 (RX)

### 问题2: 串口无法初始化
**可能原因**:
- UART_NUM_0 已被其他组件占用
- 引脚配置错误
- 驱动安装失败

**解决方案**:
- 检查是否有其他组件使用 UART_NUM_0
- 确认引脚配置正确
- 查看错误日志

### 问题3: 无法接收到数据
**可能原因**:
- 串口工具配置错误
- 波特率不匹配
- 引脚连接错误
- 数据格式不正确

**解决方案**:
1. 确认串口工具配置：
   - 波特率: 115200
   - 数据位: 8
   - 停止位: 1
   - 校验位: 无

2. 检查硬件连接：
   - TX -> RX
   - RX -> TX
   - GND -> GND

3. 测试数据发送：
   ```
   AT+VER
   AT+ENTER_TEST
   ```

### 问题4: 数据发送失败
**可能原因**:
- 日志级别设置错误
- 串口工具配置错误

**解决方案**:
1. 确认日志级别设置：
   ```
   CONFIG_LOG_DEFAULT_LEVEL_INFO=y
   ```

2. 检查串口输出格式：
   ```
   E (FT) FT_DATA: <数据内容>
   ```

## 调试命令

### 1. 检查串口状态
```bash
# 在设备启动后，通过串口发送
AT+VER
```

### 2. 测试数据接收
```bash
# 发送测试命令
AT+ENTER_TEST
```

### 3. 检查IO测试
```bash
# 启动IO测试
AT+IOTEST=1,2,100,10
# 查询结果
AT+IOTEST?
```

## 日志分析

### 正常启动日志
```
I (FT) Starting factory test, mode: 1
I (FT) Initializing factory test UART on UART_NUM_0
I (FT) TX Pin: 1, RX Pin: 3
I (FT) Factory Test UART initialized successfully
I (FT) Testing UART communication...
E (FT) FT_DATA: UART_TEST: Hello World
I (FT) Factory test task started, waiting for data...
```

### 数据接收日志
```
I (FT) RX[5]: AT+VER
I (FT) Received version query command
E (FT) FT_DATA: +VER 1.0.0,1.0.0,auth ok
```

## 硬件连接建议

### 标准连接
- 设备 TX (GPIO_20) -> 串口工具 RX
- 设备 RX (GPIO_19) -> 串口工具 TX
- 设备 GND -> 串口工具 GND

### 测试工具
推荐使用以下串口工具：
- PuTTY
- Tera Term
- Serial Studio
- Arduino IDE 串口监视器

## 故障排除清单

- [ ] 编译无错误
- [ ] 串口初始化成功
- [ ] 引脚配置正确
- [ ] 波特率匹配 (115200)
- [ ] 硬件连接正确
- [ ] 串口工具配置正确
- [ ] 日志级别设置正确
- [ ] 数据格式正确 (\r\n) 