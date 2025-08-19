#! /bin/bash -e
#XTENSA_ADDR2LINE=/home/zmz/.espressif/tools/riscv32-esp-elf/1.24.0.123_64eb9ff-8.4.0/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line
#XTENSA_ADDR2LINE=~/.espressif/tools/riscv32-esp-elf/esp-2021r2-patch5-8.4.0/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line
# XTENSA_ADDR2LINE=~/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20240530/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line
XTENSA_ADDR2LINE=~/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line
ELF_FILE=./build/xiaozhi.elf

## Validate commandline arguments
#if [ -z "$1" ]; then
#    echo "usage: $0 addr" 1>&2
#    exit 1
#fi
#ADDR="$1"
#
#$XTENSA_ADDR2LINE -e $ELF_FILE $ADDR

# 验证命令行参数
if [ $# -eq 0 ]; then
    echo "用法: \$0 addr1 addr2 ..." 1>&2
    exit 1
fi

# 筛选以"0x"开头的参数
FILTERED_ARGS=()
for arg in "$@"; do
    if [[ $arg =~ ^0x ]]; then
        FILTERED_ARGS+=("$arg")
    fi
done

$XTENSA_ADDR2LINE -e "$ELF_FILE" -a -p "${FILTERED_ARGS[@]}"