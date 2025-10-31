@echo off
setlocal EnableDelayedExpansion

:: 设置命令行编码为 UTF-8
chcp 65001 > nul

:: 获取 MAC 地址
for /f "tokens=2 delims= " %%i in ('esptool.exe read_mac ^| findstr "MAC:"') do (
    :: 移除冒号并存储结果
    set "MAC=%%i"
    set "MAC=!MAC::=!"
    
    :: 输出结果并退出
    echo !MAC!
    exit /b 0
)

:: 如果没有找到 MAC 地址
echo Failed to get MAC address >&2
exit /b 1 