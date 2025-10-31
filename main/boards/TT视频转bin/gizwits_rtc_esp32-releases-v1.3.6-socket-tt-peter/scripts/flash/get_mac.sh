#!/bin/bash

# 获取 MAC 地址并处理格式 - 只取第一个匹配的 MAC
MAC=$(esptool.py read_mac | grep "MAC:" | head -n 1 | cut -d' ' -f2 | tr -d ':')

if [ -n "$MAC" ]; then
    echo $MAC
    exit 0
else
    echo "Failed to get MAC address" >&2
    exit 1
fi 