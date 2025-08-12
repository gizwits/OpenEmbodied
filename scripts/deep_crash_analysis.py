#!/usr/bin/env python3
"""
ESP32深度崩溃分析脚本
尝试从栈内存中提取应用层的崩溃信息
"""

import os
import sys
import subprocess
import re
from pathlib import Path

class DeepCrashAnalyzer:
    def __init__(self, elf_path, addr2line_path="riscv32-esp-elf-addr2line"):
        self.elf_path = elf_path
        self.addr2line_path = addr2line_path
        self.addr2line_cache = {}
        
    def addr2line(self, address):
        """使用addr2line转换地址"""
        if address in self.addr2line_cache:
            return self.addr2line_cache[address]
            
        try:
            clean_addr = address.replace('0x', '')
            result = subprocess.run([
                self.addr2line_path, 
                '-e', self.elf_path, 
                clean_addr
            ], capture_output=True, text=True)
            
            if result.returncode == 0:
                output = result.stdout.strip()
                if output and output != '??:0':
                    self.addr2line_cache[address] = output
                    return output
                else:
                    self.addr2line_cache[address] = "未知位置"
                    return "未知位置"
            else:
                self.addr2line_cache[address] = f"错误: {result.stderr.strip()}"
                return f"错误: {result.stderr.strip()}"
                
        except Exception as e:
            error_msg = f"执行错误: {str(e)}"
            self.addr2line_cache[address] = error_msg
            return error_msg
    
    def analyze_stack_memory(self, log_file):
        """深度分析栈内存内容"""
        print("🔍 深度栈内存分析")
        print("=" * 60)
        
        with open(log_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 查找栈内存部分
        stack_section = re.search(r'Stack memory:(.*?)(?=\n\n|\n[^0-9a-f]|$)', 
                                 content, re.DOTALL)
        
        if not stack_section:
            print("未找到栈内存内容")
            return
        
        stack_content = stack_section.group(1)
        lines = stack_content.strip().split('\n')
        
        print(f"找到 {len(lines)} 行栈内存数据")
        print()
        
        # 分析栈中的地址模式
        all_addresses = []
        for line in lines:
            if line.strip():
                # 提取每行中的地址
                addresses = re.findall(r'0x[0-9a-fA-F]{8}', line)
                all_addresses.extend(addresses)
        
        print(f"栈中发现 {len(all_addresses)} 个地址")
        print()
        
        # 按地址范围分类
        app_addresses = []
        system_addresses = []
        unknown_addresses = []
        
        for addr in all_addresses:
            location = self.addr2line(addr)
            if "esp-idf" in location or "components" in location:
                system_addresses.append((addr, location))
            elif "??:0" in location or "未知位置" in location:
                unknown_addresses.append((addr, location))
            else:
                app_addresses.append((addr, location))
        
        # 显示应用层地址（这些可能是你的代码）
        if app_addresses:
            print("🎯 可能的应用层地址 (你的代码):")
            print("-" * 40)
            for addr, location in app_addresses[:10]:  # 只显示前10个
                print(f"  {addr} -> {location}")
            print()
        
        # 显示系统层地址
        if system_addresses:
            print("🔧 系统层地址 (ESP-IDF):")
            print("-" * 40)
            for addr, location in system_addresses[:10]:
                print(f"  {addr} -> {location}")
            print()
        
        # 显示未知地址（可能是数据或代码）
        if unknown_addresses:
            print("❓ 未知地址 (可能是数据或代码):")
            print("-" * 40)
            for addr, location in unknown_addresses[:10]:
                print(f"  {addr} -> {location}")
            print()
        
        # 尝试找到调用链模式
        self.find_call_chain_pattern(all_addresses)
    
    def find_call_chain_pattern(self, addresses):
        """尝试找到调用链模式"""
        print("🔗 调用链模式分析:")
        print("-" * 40)
        
        # 按地址大小排序
        sorted_addrs = sorted(addresses, key=lambda x: int(x, 16))
        
        # 查找连续的地址（可能是调用链）
        call_chains = []
        for i in range(len(sorted_addrs) - 1):
            addr1 = int(sorted_addrs[i], 16)
            addr2 = int(sorted_addrs[i + 1], 16)
            
            # 如果两个地址相差较小，可能是相关的
            if 0 < (addr2 - addr1) < 0x1000:  # 4KB范围内
                location1 = self.addr2line(sorted_addrs[i])
                location2 = self.addr2line(sorted_addrs[i + 1])
                
                if "??:0" not in location1 and "??:0" not in location2:
                    call_chains.append((sorted_addrs[i], sorted_addrs[i + 1], location1, location2))
        
        if call_chains:
            print(f"发现 {len(call_chains)} 个可能的调用链:")
            for i, (addr1, addr2, loc1, loc2) in enumerate(call_chains[:5]):
                print(f"  链 {i+1}: {addr1} -> {addr2}")
                print(f"        {loc1}")
                print(f"        {loc2}")
                print()
        else:
            print("未发现明显的调用链模式")
    
    def analyze_crash_context(self, log_file):
        """分析崩溃上下文"""
        print("📋 崩溃上下文分析:")
        print("=" * 60)
        
        with open(log_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 分析寄存器状态
        print("寄存器状态分析:")
        reg_pattern = r'(MEPC|RA|SP|GP|TP|S0/FP|S1|A0|A1|A2|A3|A4|A5|A6|A7|S2|S3|S4|S5|S6|S7|S8|S9|S10|S11|T3|T4|T5|T6)\s+:\s+(0x[0-9a-fA-F]+)'
        reg_matches = re.findall(reg_pattern, content)
        
        for reg, addr in reg_matches:
            location = self.addr2line(addr)
            print(f"  {reg:6}: {addr} -> {location}")
        
        print()
        
        # 分析异常信息
        print("异常信息分析:")
        mepc_match = re.search(r'MEPC\s+:\s+(0x[0-9a-fA-F]+)', content)
        mcause_match = re.search(r'MCAUSE\s+:\s+(0x[0-9a-fA-F]+)', content)
        mtval_match = re.search(r'MTVAL\s+:\s+(0x[0-9a-fA-F]+)', content)
        
        if mepc_match:
            mepc_addr = mepc_match.group(1)
            mepc_loc = self.addr2line(mepc_addr)
            print(f"  MEPC (异常PC): {mepc_addr} -> {mepc_loc}")
        
        if mcause_match:
            mcause = int(mcause_match.group(1), 16)
            print(f"  MCAUSE (异常原因): 0x{mcause:x} ({mcause})")
            if mcause == 2:
                print("    原因: 非法指令异常")
            elif mcause == 3:
                print("    原因: 断点异常")
            elif mcause == 4:
                print("    原因: 加载地址未对齐异常")
            elif mcause == 5:
                print("    原因: 存储地址未对齐异常")
            elif mcause == 6:
                print("    原因: 环境调用异常")
            elif mcause == 7:
                print("    原因: 环境调用异常")
            elif mcause == 11:
                print("    原因: 环境调用异常")
            else:
                print(f"    原因: 未知异常类型")
        
        if mtval_match:
            mtval = mtval_match.group(1)
            print(f"  MTVAL (异常值): {mtval}")
        
        print()
    
    def generate_recommendations(self):
        """生成调试建议"""
        print("💡 调试建议:")
        print("=" * 60)
        
        print("1. 🔍 启用更详细的崩溃信息:")
        print("   在 sdkconfig 中添加:")
        print("   CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y")
        print("   CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y")
        print("   CONFIG_ESP_SYSTEM_PANIC_GDBSTUB_RETURN=y")
        print()
        
        print("2. 🐛 启用更多调试选项:")
        print("   在 CMakeLists.txt 中添加:")
        print("   set(CMAKE_BUILD_TYPE Debug)")
        print("   set(COMPILER_OPTIMIZATION_DEBUG_INFO \"-g\")")
        print("   set(COMPILER_OPTIMIZATION_DEBUG_INFO \"-O0\")")
        print()
        
        print("3. 📝 添加更多日志记录:")
        print("   在关键代码位置添加:")
        print("   ESP_LOGI(TAG, \"Function: %s, Line: %d\", __FUNCTION__, __LINE__);")
        print()
        
        print("4. 🚨 检查UBSan检测到的问题:")
        print("   - 数组越界访问")
        print("   - 未初始化变量使用")
        print("   - 空指针解引用")
        print("   - 类型转换问题")
        print()
        
        print("5. 🔧 使用GDB调试:")
        print("   idf.py gdb")
        print("   在崩溃点设置断点")
        print()
        
        print("6. 📊 分析内存使用:")
        print("   检查是否有内存泄漏或栈溢出")
        print("   使用 heap_caps_print_heap_info() 监控内存")
    
    def analyze(self, log_file):
        """执行完整分析"""
        if not os.path.exists(self.elf_path):
            print(f"❌ ELF文件不存在: {self.elf_path}")
            return
        
        print("🚀 ESP32深度崩溃分析开始")
        print("=" * 60)
        print(f"崩溃日志: {log_file}")
        print(f"ELF文件: {self.elf_path}")
        print()
        
        self.analyze_crash_context(log_file)
        self.analyze_stack_memory(log_file)
        self.generate_recommendations()
        
        print("\n✅ 分析完成!")

def main():
    if len(sys.argv) < 2:
        print("用法: python3 deep_crash_analysis.py <crash.log> [elf_file]")
        print("示例: python3 deep_crash_analysis.py crash.log build/xiaozhi.elf")
        sys.exit(1)
    
    log_file = sys.argv[1]
    elf_file = sys.argv[2] if len(sys.argv) > 2 else "build/xiaozhi.elf"
    
    if not os.path.exists(log_file):
        print(f"❌ 崩溃日志文件不存在: {log_file}")
        sys.exit(1)
    
    analyzer = DeepCrashAnalyzer(elf_file)
    analyzer.analyze(log_file)

if __name__ == "__main__":
    main()
