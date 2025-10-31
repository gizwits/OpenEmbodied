#!/usr/bin/env python3
"""
将日志文件中的十六进制核心转储数据转换为ESP-IDF可识别的格式。

用法:
    python3 convert_coredump.py <log_file> <output_file> [--format {elf,raw,b64}]

参数:
    log_file: 包含核心转储数据的日志文件
    output_file: 输出文件路径
    --format: 输出格式，可选 elf, raw, b64，默认为 b64
"""

import sys
import re
import argparse
import base64
import binascii
import os
import json

def extract_hex_data_from_log(log_file):
    """从日志文件中提取十六进制核心转储数据"""
    print(f"正在从 {log_file} 提取核心转储数据...")
    
    # 尝试不同的模式来匹配核心转储数据
    patterns = [
        # 模式1: 寻找 "Complete core dump, size: X bytes" 后面的十六进制数据
        r"Complete core dump, size: \d+ bytes.*?message,([0-9a-fA-F]+)",
        # 模式2: 寻找 "core_dump_data" 字段后面的十六进制数据
        r"core_dump_data\":\"([0-9a-fA-F]+)\"",
        # 模式3: 寻找任何长的十六进制字符串 (至少1000个字符)
        r"([0-9a-fA-F]{1000,})"
    ]
    
    with open(log_file, 'r', errors='ignore') as f:
        content = f.read()
    
    for pattern in patterns:
        matches = re.findall(pattern, content, re.DOTALL)
        if matches:
            # 使用最长的匹配结果
            hex_data = max(matches, key=len)
            print(f"找到核心转储数据，长度: {len(hex_data)} 字符 ({len(hex_data)//2} 字节)")
            return hex_data
    
    # 尝试从JSON中提取
    try:
        json_matches = re.findall(r'\{.*\}', content, re.DOTALL)
        for json_str in json_matches:
            try:
                data = json.loads(json_str)
                if isinstance(data, dict):
                    # 检查是否有核心转储数据
                    if 'stack' in data:
                        hex_data = data['stack']
                        if isinstance(hex_data, str) and all(c in '0123456789abcdefABCDEF' for c in hex_data):
                            print(f"从JSON中找到核心转储数据，长度: {len(hex_data)} 字符")
                            return hex_data
                    
                    # 递归检查嵌套字典
                    def search_dict(d):
                        for k, v in d.items():
                            if isinstance(v, str) and len(v) > 1000 and all(c in '0123456789abcdefABCDEF' for c in v):
                                return v
                            elif isinstance(v, dict):
                                result = search_dict(v)
                                if result:
                                    return result
                            elif isinstance(v, list):
                                for item in v:
                                    if isinstance(item, dict):
                                        result = search_dict(item)
                                        if result:
                                            return result
                        return None
                    
                    result = search_dict(data)
                    if result:
                        print(f"从JSON嵌套字典中找到核心转储数据，长度: {len(result)} 字符")
                        return result
            except json.JSONDecodeError:
                pass
    except Exception as e:
        print(f"尝试解析JSON时出错: {e}")
    
    # 尝试直接搜索十六进制字符串
    hex_chunks = []
    lines = content.split('\n')
    for line in lines:
        # 查找可能的十六进制数据行
        hex_match = re.search(r'([0-9a-fA-F]{16,})', line)
        if hex_match:
            hex_chunks.append(hex_match.group(1))
    
    if hex_chunks:
        # 尝试连接所有找到的十六进制块
        combined_hex = ''.join(hex_chunks)
        if len(combined_hex) > 1000:  # 确保数据足够长
            print(f"通过组合多行找到核心转储数据，长度: {len(combined_hex)} 字符")
            return combined_hex
    
    print("错误: 未找到核心转储数据")
    return None

def hex_to_binary(hex_data):
    """将十六进制字符串转换为二进制数据"""
    try:
        # 确保十六进制字符串长度是偶数
        if len(hex_data) % 2 != 0:
            hex_data = hex_data[:-1]
        
        binary_data = binascii.unhexlify(hex_data)
        print(f"成功将十六进制转换为二进制，大小: {len(binary_data)} 字节")
        return binary_data
    except binascii.Error as e:
        print(f"错误: 无法将十六进制转换为二进制: {e}")
        
        # 尝试清理十六进制字符串
        cleaned_hex = ''.join(c for c in hex_data if c in '0123456789abcdefABCDEF')
        if len(cleaned_hex) % 2 != 0:
            cleaned_hex = cleaned_hex[:-1]
        
        try:
            binary_data = binascii.unhexlify(cleaned_hex)
            print(f"清理后成功转换，大小: {len(binary_data)} 字节")
            return binary_data
        except binascii.Error as e:
            print(f"清理后仍然无法转换: {e}")
            return None

def save_as_format(binary_data, output_file, format_type):
    """将二进制数据保存为指定格式"""
    if format_type == 'raw':
        with open(output_file, 'wb') as f:
            f.write(binary_data)
        print(f"已保存为原始二进制格式: {output_file} ({len(binary_data)} 字节)")
        
    elif format_type == 'b64':
        b64_data = base64.b64encode(binary_data).decode('ascii')
        with open(output_file, 'w') as f:
            f.write(b64_data)
        print(f"已保存为Base64格式: {output_file} ({len(b64_data)} 字符)")
        
    elif format_type == 'elf':
        print("警告: 直接生成ELF格式需要更复杂的处理，将保存为原始二进制格式")
        with open(output_file, 'wb') as f:
            f.write(binary_data)
        print(f"已保存为原始二进制格式: {output_file} ({len(binary_data)} 字节)")
    
    return True

def main():
    parser = argparse.ArgumentParser(description='将日志文件中的十六进制核心转储数据转换为ESP-IDF可识别的格式')
    parser.add_argument('log_file', help='包含核心转储数据的日志文件')
    parser.add_argument('output_file', help='输出文件路径')
    parser.add_argument('--format', choices=['elf', 'raw', 'b64'], default='b64',
                        help='输出格式，可选 elf, raw, b64，默认为 b64')
    parser.add_argument('--expected-size', type=int, default=0,
                        help='预期的核心转储大小（字节），用于验证')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.log_file):
        print(f"错误: 文件 {args.log_file} 不存在")
        return 1
    
    # 提取十六进制数据
    hex_data = extract_hex_data_from_log(args.log_file)
    if not hex_data:
        return 1
    
    # 转换为二进制
    binary_data = hex_to_binary(hex_data)
    if not binary_data:
        return 1
    
    # 验证大小
    if args.expected_size > 0 and len(binary_data) != args.expected_size:
        print(f"警告: 转换后的数据大小 ({len(binary_data)} 字节) 与预期大小 ({args.expected_size} 字节) 不符")
        
        # 如果数据太小，尝试填充
        if len(binary_data) < args.expected_size:
            padding = b'\x00' * (args.expected_size - len(binary_data))
            binary_data += padding
            print(f"已填充数据至预期大小: {len(binary_data)} 字节")
    
    # 保存为指定格式
    if save_as_format(binary_data, args.output_file, args.format):
        print("\n转换完成!")
        print(f"使用以下命令分析核心转储: idf.py coredump-info -c {args.output_file} -t {args.format}")
        return 0
    else:
        return 1

if __name__ == "__main__":
    sys.exit(main()) 