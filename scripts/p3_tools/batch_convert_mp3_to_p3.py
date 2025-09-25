#!/usr/bin/env python3
"""
批量转换MP3文件为P3格式
自动处理assets目录中的所有mp3文件
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path

def find_mp3_files(directory):
    """查找目录中的所有mp3文件"""
    mp3_files = []
    for root, dirs, files in os.walk(directory):
        for file in files:
            if file.lower().endswith('.mp3'):
                mp3_files.append(os.path.join(root, file))
    return mp3_files

def convert_mp3_to_p3(input_file, output_file, lufs=-16.0, disable_loudnorm=False):
    """转换单个mp3文件为p3格式"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    convert_script = os.path.join(script_dir, 'convert_audio_to_p3.py')
    
    cmd = [sys.executable, convert_script, input_file, output_file]
    
    if disable_loudnorm:
        cmd.append('-d')
    else:
        cmd.extend(['-l', str(lufs)])
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return True, result.stdout
    except subprocess.CalledProcessError as e:
        return False, e.stderr

def main():
    parser = argparse.ArgumentParser(description='批量转换MP3文件为P3格式')
    parser.add_argument('input_dir', nargs='?', default='assets',
                       help='输入目录路径 (默认: assets)')
    parser.add_argument('output_dir', nargs='?', default='./',
                       help='输出目录路径 (默认: ../main/assets/zh-CN)')
    parser.add_argument('-l', '--lufs', type=float, default=-16.0,
                       help='目标响度 LUFS (默认: -16)')
    parser.add_argument('-d', '--disable-loudnorm', action='store_true',
                       help='禁用响度标准化')
    parser.add_argument('--dry-run', action='store_true',
                       help='预览模式，只显示将要转换的文件，不实际转换')
    parser.add_argument('--overwrite', action='store_true',
                       help='覆盖已存在的输出文件')
    
    args = parser.parse_args()
    
    # 获取脚本所在目录
    script_dir = os.path.dirname(os.path.abspath(__file__))
    input_dir = os.path.join(script_dir, args.input_dir)
    output_dir = os.path.join(script_dir, args.output_dir)
    
    # 检查输入目录
    if not os.path.exists(input_dir):
        print(f"错误: 输入目录不存在: {input_dir}")
        return 1
    
    # 创建输出目录
    os.makedirs(output_dir, exist_ok=True)
    
    # 查找所有mp3文件
    mp3_files = find_mp3_files(input_dir)
    
    if not mp3_files:
        print(f"在目录 {input_dir} 中未找到MP3文件")
        return 0
    
    print(f"找到 {len(mp3_files)} 个MP3文件:")
    for mp3_file in mp3_files:
        print(f"  - {os.path.relpath(mp3_file, script_dir)}")
    
    if args.dry_run:
        print("\n预览模式 - 将要转换的文件:")
        for mp3_file in mp3_files:
            # 生成输出文件名
            rel_path = os.path.relpath(mp3_file, input_dir)
            base_name = os.path.splitext(rel_path)[0]
            output_file = os.path.join(output_dir, f"{base_name}.p3")
            print(f"  {os.path.relpath(mp3_file, script_dir)} -> {os.path.relpath(output_file, script_dir)}")
        return 0
    
    print(f"\n开始转换...")
    print(f"输出目录: {os.path.relpath(output_dir, script_dir)}")
    print(f"响度设置: {'禁用' if args.disable_loudnorm else f'{args.lufs} LUFS'}")
    print("-" * 50)
    
    success_count = 0
    error_count = 0
    
    for i, mp3_file in enumerate(mp3_files, 1):
        # 生成输出文件名
        rel_path = os.path.relpath(mp3_file, input_dir)
        base_name = os.path.splitext(rel_path)[0]
        output_file = os.path.join(output_dir, f"{base_name}.p3")
        
        print(f"[{i}/{len(mp3_files)}] 转换: {os.path.basename(mp3_file)}")
        
        # 检查输出文件是否已存在
        if os.path.exists(output_file) and not args.overwrite:
            print(f"  跳过: 输出文件已存在 {os.path.basename(output_file)}")
            continue
        
        # 转换文件
        success, message = convert_mp3_to_p3(mp3_file, output_file, args.lufs, args.disable_loudnorm)
        
        if success:
            print(f"  ✓ 成功: {os.path.basename(output_file)}")
            success_count += 1
        else:
            print(f"  ✗ 失败: {message.strip()}")
            error_count += 1
    
    print("-" * 50)
    print(f"转换完成: 成功 {success_count} 个, 失败 {error_count} 个")
    
    if error_count > 0:
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
