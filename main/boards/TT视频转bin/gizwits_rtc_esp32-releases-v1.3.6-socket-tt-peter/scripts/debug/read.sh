#!/bin/bash

# 设置路径和文件名
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUMP_PATH="${SCRIPT_DIR}/dump"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
FILE_NAME="dump_${TIMESTAMP}"

echo "DumpPath = ${DUMP_PATH}"
echo "FileName = ${FILE_NAME}"

# 创建目录（如果不存在）
if [ ! -d "${DUMP_PATH}" ]; then
    mkdir -p "${DUMP_PATH}"
fi

# 设置读取地址和大小
READ_ADDRESS="0x744000"
READ_SIZE="0x2000"

echo "xxd -g2 -c32 dump/${FILE_NAME}.bin > dump/${FILE_NAME}.txt"
echo "esptool.py --chip esp32s3 -b 921600 read_flash ${READ_ADDRESS} ${READ_SIZE} \"${DUMP_PATH}/${FILE_NAME}.bin\""

# 执行读取闪存命令
esptool.py --chip esp32s3 -b 921600 --after no_reset read_flash ${READ_ADDRESS} ${READ_SIZE} "${DUMP_PATH}/${FILE_NAME}.bin"

# 可选：使用 xxd 生成十六进制转储
# xxd -g2 -c32 "${DUMP_PATH}/${FILE_NAME}.bin" > "${DUMP_PATH}/${FILE_NAME}.txt" 