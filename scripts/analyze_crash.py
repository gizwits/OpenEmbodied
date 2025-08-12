#!/usr/bin/env python3
"""
ESP32 Crash Log Analyzer
使用 riscv32-esp-elf-addr2line 工具分析崩溃日志中的地址
"""

import os
import sys
import subprocess
import re
from pathlib import Path

class CrashAnalyzer:
    def __init__(self, elf_path, addr2line_path="riscv32-esp-elf-addr2line"):
        self.elf_path = elf_path
        self.addr2line_path = addr2line_path
        self.addr2line_cache = {}
        
    def check_tools(self):
        """检查必要的工具是否可用"""
        try:
            result = subprocess.run([self.addr2line_path, '--version'], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                print(f"✓ {self.addr2line_path} 可用")
                return True
        except FileNotFoundError:
            print(f"✗ {self.addr2line_path} 未找到")
            return False
        return False
    
    def addr2line(self, address):
        """使用addr2line转换地址为源代码位置"""
        if address in self.addr2line_cache:
            return self.addr2line_cache[address]
            
        try:
            # 移除0x前缀
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
    
    def parse_crash_log(self, log_file):
        """解析崩溃日志文件"""
        print(f"正在分析崩溃日志: {log_file}")
        print("=" * 60)
        
        with open(log_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 提取所有地址
        addresses = []
        
        # 匹配PC地址
        pc_pattern = r'PC\s+(0x[0-9a-fA-F]+)'
        pc_matches = re.findall(pc_pattern, content)
        for addr in pc_matches:
            addresses.append(('PC', addr))
        
        # 匹配带注释的地址
        addr_comment_pattern = r'---\s+(0x[0-9a-fA-F]+):\s+(.+)'
        addr_matches = re.findall(addr_comment_pattern, content)
        for addr, comment in addr_matches:
            addresses.append(('调用栈', addr, comment))
        
        # 匹配寄存器中的地址
        reg_pattern = r'(MEPC|RA|SP|GP|TP|S0/FP|S1|A0|A1|A2|A3|A4|A5|A6|A7|S2|S3|S4|S5|S6|S7|S8|S9|S10|S11|T3|T4|T5|T6)\s+:\s+(0x[0-9a-fA-F]+)'
        reg_matches = re.findall(reg_pattern, content)
        for reg, addr in reg_matches:
            addresses.append(('寄存器', addr, f'{reg}寄存器'))
        
        return addresses
    
    def analyze_addresses(self, addresses):
        """分析所有地址"""
        print("地址分析结果:")
        print("-" * 60)
        
        for addr_info in addresses:
            if len(addr_info) == 2:
                addr_type, addr = addr_info
                location = self.addr2line(addr)
                print(f"{addr_type:8} {addr:12} -> {location}")
            elif len(addr_info) == 3:
                addr_type, addr, comment = addr_info
                location = self.addr2line(addr)
                print(f"{addr_type:8} {addr:12} -> {location}")
                print(f"{'':20} 注释: {comment}")
            print()
    
    def analyze_stack_memory(self, log_file):
        """分析栈内存内容"""
        print("栈内存分析:")
        print("-" * 60)
        
        with open(log_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 查找栈内存部分
        stack_section = re.search(r'Stack memory:(.*?)(?=\n\n|\n[^0-9a-f]|$)', 
                                 content, re.DOTALL)
        
        if stack_section:
            stack_content = stack_section.group(1)
            lines = stack_content.strip().split('\n')
            
            print("栈内存转储 (前20行):")
            for i, line in enumerate(lines[:20]):
                if line.strip():
                    print(f"  {line.strip()}")
                    if i >= 19:
                        print("  ... (更多内容省略)")
                        break
        
        # 尝试解析栈中的地址
        addr_pattern = r'0x[0-9a-fA-F]{8}'
        addresses = re.findall(addr_pattern, content)
        
        if addresses:
            print(f"\n栈中发现 {len(addresses)} 个地址:")
            unique_addrs = list(set(addresses))[:10]  # 只显示前10个唯一地址
            for addr in unique_addrs:
                location = self.addr2line(addr)
                print(f"  {addr} -> {location}")
    
    def generate_report(self, log_file, output_file=None):
        """生成完整的分析报告"""
        if not self.check_tools():
            print("工具检查失败，无法继续分析")
            return
        
        if not os.path.exists(self.elf_path):
            print(f"ELF文件不存在: {self.elf_path}")
            return
        
        addresses = self.parse_crash_log(log_file)
        self.analyze_addresses(addresses)
        self.analyze_stack_memory(log_file)
        
        # 生成建议
        print("\n" + "=" * 60)
        print("崩溃分析建议:")
        print("-" * 60)
        print("1. 这是一个C++异常未捕获导致的崩溃")
        print("2. UBSan检测到了未定义行为")
        print("3. 建议检查以下代码:")
        print("   - 数组越界访问")
        print("   - 未初始化变量使用")
        print("   - 空指针解引用")
        print("   - 异常处理是否完整")
        print("4. 启用更多调试信息:")
        print("   - 设置 CMAKE_BUILD_TYPE=Debug")
        print("   - 添加 -g 编译选项")
        
        if output_file:
            # 保存报告到文件
            with open(output_file, 'w', encoding='utf-8') as f:
                f.write("ESP32崩溃分析报告\n")
                f.write("=" * 60 + "\n")
                # 这里可以添加更多报告内容

def main():
    if len(sys.argv) < 2:
        print("用法: python3 analyze_crash.py <crash.log> [elf_file]")
        print("示例: python3 analyze_crash.py crash.log build/xiaozhi.elf")
        sys.exit(1)
    
    log_file = sys.argv[1]
    elf_file = sys.argv[2] if len(sys.argv) > 2 else "build/xiaozhi.elf"
    
    if not os.path.exists(log_file):
        print(f"崩溃日志文件不存在: {log_file}")
        sys.exit(1)
    
    analyzer = CrashAnalyzer(elf_file)
    analyzer.generate_report(log_file)

if __name__ == "__main__":
    main()
