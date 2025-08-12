#!/bin/bash
# ESP32崩溃日志地址转换脚本
# 使用方法: ./addr2line_quick.sh <crash.log> [elf_file]

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 检查参数
if [ $# -lt 1 ]; then
    echo -e "${RED}用法: $0 <crash.log> [elf_file]${NC}"
    echo "示例: $0 crash.log build/xiaozhi.elf"
    exit 1
fi

CRASH_LOG="$1"
ELF_FILE="${2:-build/xiaozhi.elf}"

# 检查文件是否存在
if [ ! -f "$CRASH_LOG" ]; then
    echo -e "${RED}错误: 崩溃日志文件不存在: $CRASH_LOG${NC}"
    exit 1
fi

if [ ! -f "$ELF_FILE" ]; then
    echo -e "${RED}错误: ELF文件不存在: $ELF_FILE${NC}"
    exit 1
fi

# 检查addr2line工具
if ! command -v riscv32-esp-elf-addr2line &> /dev/null; then
    echo -e "${RED}错误: riscv32-esp-elf-addr2line 工具未找到${NC}"
    echo "请确保ESP-IDF环境已正确设置"
    exit 1
fi

echo -e "${GREEN}ESP32崩溃日志地址分析${NC}"
echo "=================================="
echo "崩溃日志: $CRASH_LOG"
echo "ELF文件: $ELF_FILE"
echo ""

# 提取并转换PC地址
echo -e "${BLUE}PC地址分析:${NC}"
grep "PC 0x" "$CRASH_LOG" | while read -r line; do
    addr=$(echo "$line" | grep -o "0x[0-9a-fA-F]*")
    if [ -n "$addr" ]; then
        echo -n "PC $addr -> "
        riscv32-esp-elf-addr2line -e "$ELF_FILE" "$addr"
    fi
done
echo ""

# 提取并转换调用栈地址
echo -e "${BLUE}调用栈分析:${NC}"
grep "--- 0x" "$CRASH_LOG" | while read -r line; do
    addr=$(echo "$line" | grep -o "0x[0-9a-fA-F]*")
    comment=$(echo "$line" | sed 's/.*--- 0x[0-9a-fA-F]*: //')
    if [ -n "$addr" ]; then
        echo -n "$addr -> "
        riscv32-esp-elf-addr2line -e "$ELF_FILE" "$addr"
        echo "  注释: $comment"
    fi
done
echo ""

# 提取并转换关键寄存器地址
echo -e "${BLUE}关键寄存器分析:${NC}"
grep -E "(MEPC|RA|SP|GP)" "$CRASH_LOG" | while read -r line; do
    reg=$(echo "$line" | grep -o "[A-Z]*")
    addr=$(echo "$line" | grep -o "0x[0-9a-fA-F]*")
    if [ -n "$addr" ]; then
        echo -n "$reg $addr -> "
        riscv32-esp-elf-addr2line -e "$ELF_FILE" "$addr"
    fi
done
echo ""

# 分析栈内存中的地址
echo -e "${BLUE}栈内存地址分析 (前10个):${NC}"
grep -o "0x[0-9a-fA-F]*" "$CRASH_LOG" | head -10 | while read -r addr; do
    echo -n "$addr -> "
    riscv32-esp-elf-addr2line -e "$ELF_FILE" "$addr"
done

echo ""
echo -e "${GREEN}分析完成!${NC}"
echo ""
echo -e "${YELLOW}崩溃分析建议:${NC}"
echo "1. 这是一个C++异常未捕获导致的崩溃"
echo "2. UBSan检测到了未定义行为"
echo "3. 建议检查数组越界、未初始化变量、空指针等问题"
echo "4. 启用Debug编译模式获取更多信息"
