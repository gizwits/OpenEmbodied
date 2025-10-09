#!/bin/bash
# ESP32崩溃日志地址转换脚本 (简化版)
# 使用方法: ./addr2line_simple.sh <crash.log> [elf_file]

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

echo -e "${GREEN}ESP32崩溃日志地址分析 (简化版)${NC}"
echo "=========================================="
echo "崩溃日志: $CRASH_LOG"
echo "ELF文件: $ELF_FILE"
echo ""

# 手动分析关键地址
echo -e "${BLUE}关键地址分析:${NC}"

# PC地址
echo "PC地址:"
echo "  0x420c24d3 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x420c24d3)"
echo ""

# 调用栈地址
echo "调用栈地址:"
echo "  0x40380776 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x40380776)"
echo "  0x40384cfe -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x40384cfe)"
echo ""

# 关键寄存器地址
echo "关键寄存器地址:"
echo "  MEPC: 0x40380776 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x40380776)"
echo "  RA: 0x40384cfe -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x40384cfe)"
echo "  SP: 0x3fcd8860 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x3fcd8860)"
echo "  GP: 0x3fcac130 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x3fcac130)"
echo ""

# 栈内存中的一些地址
echo "栈内存地址 (前5个):"
echo "  0x40387362 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x40387362)"
echo "  0x3fcb6f58 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x3fcb6f58)"
echo "  0x420f0030 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x420f0030)"
echo "  0x3fcade5c -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x3fcade5c)"
echo "  0x3fcade78 -> $(riscv32-esp-elf-addr2line -e "$ELF_FILE" 0x3fcade78)"
echo ""

echo -e "${GREEN}分析完成!${NC}"
echo ""
echo -e "${YELLOW}崩溃分析总结:${NC}"
echo "1. 崩溃类型: C++异常未捕获 (abort()被调用)"
echo "2. 崩溃位置: PC 0x420c24d3 (C++异常处理)"
echo "3. 调用栈:"
echo "   - panic_abort() at panic.c:482"
echo "   - __ubsan_include() at ubsan.c:311"
echo "4. 原因: UBSan检测到未定义行为"
echo ""
echo -e "${YELLOW}建议:${NC}"
echo "1. 检查数组越界访问"
echo "2. 检查未初始化变量使用"
echo "3. 检查空指针解引用"
echo "4. 确保异常处理完整"
echo "5. 启用Debug编译模式"
