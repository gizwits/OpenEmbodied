#!/usr/bin/env python3
"""
批量转换.p3文件：
1. 将.p3文件转换为PCM_16的wav文件
2. 将wav文件转换为40ms帧的opus编码
保持原有的目录结构
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path

def find_p3_files(root_dir):
    """递归查找所有.p3文件"""
    p3_files = []
    for root, dirs, files in os.walk(root_dir):
        for file in files:
            if file.endswith('.p3'):
                p3_files.append(os.path.join(root, file))
    return p3_files

def get_relative_path(file_path, base_dir):
    """获取相对于基础目录的路径"""
    return os.path.relpath(file_path, base_dir)

def convert_p3_to_wav(p3_file, temp_dir, input_dir):
    """将.p3文件转换为wav文件"""
    # 保持目录结构
    rel_path = get_relative_path(p3_file, input_dir)
    rel_dir = os.path.dirname(rel_path)
    temp_sub_dir = os.path.join(temp_dir, rel_dir)
    os.makedirs(temp_sub_dir, exist_ok=True)
    
    wav_file = os.path.join(temp_sub_dir, os.path.basename(p3_file).replace('.p3', '.wav'))
    
    cmd = [sys.executable, 'convert_p3_to_audio.py', p3_file, wav_file]
    print(f"转换 {p3_file} -> {wav_file}")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"错误: 转换 {p3_file} 失败")
        print(f"错误信息: {result.stderr}")
        return None
    
    return wav_file

def convert_wav_to_p3_40ms(wav_file, output_dir, input_dir):
    """将wav文件转换为40ms帧的.p3文件"""
    # 保持目录结构
    rel_path = get_relative_path(wav_file, input_dir)
    rel_dir = os.path.dirname(rel_path)
    output_sub_dir = os.path.join(output_dir, rel_dir)
    os.makedirs(output_sub_dir, exist_ok=True)
    
    p3_file = os.path.join(output_sub_dir, os.path.basename(wav_file).replace('.wav', '.p3'))
    
    
    cmd = [sys.executable, 'convert_audio_to_p3.py', wav_file, p3_file, '--duration', '20']
    print(f"转换 {wav_file} -> {p3_file}")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"错误: 转换 {wav_file} 失败")
        print(f"错误信息: {result.stderr}")
        return None
    
    return p3_file

def main():
    parser = argparse.ArgumentParser(description='批量转换.p3文件为40ms帧的opus编码')
    parser.add_argument('input_dir', help='包含.p3文件的目录')
    parser.add_argument('output_dir', help='输出目录')
    parser.add_argument('--temp-dir', default='temp_wav', help='临时wav文件目录')
    parser.add_argument('--keep-temp', action='store_true', help='保留临时文件')
    
    args = parser.parse_args()
    
    # 创建输出目录和临时目录
    os.makedirs(args.output_dir, exist_ok=True)
    os.makedirs(args.temp_dir, exist_ok=True)
    
    # 查找所有.p3文件
    p3_files = find_p3_files(args.input_dir)
    print(f"找到 {len(p3_files)} 个.p3文件")
    
    if not p3_files:
        print("未找到.p3文件")
        return
    
    # 处理每个文件
    success_count = 0
    for p3_file in p3_files:
        try:
            # 第一步：转换为wav
            wav_file = convert_p3_to_wav(p3_file, args.temp_dir, args.input_dir)
            if wav_file is None:
                continue
            
            # 第二步：转换为40ms帧的.p3
            output_p3 = convert_wav_to_p3_40ms(wav_file, args.output_dir, args.temp_dir)
            if output_p3 is None:
                continue
            
            success_count += 1
            print(f"成功处理: {p3_file} -> {output_p3}")
            
        except Exception as e:
            print(f"处理 {p3_file} 时发生错误: {e}")
    
    # 清理临时文件
    if not args.keep_temp:
        import shutil
        if os.path.exists(args.temp_dir):
            shutil.rmtree(args.temp_dir)
            print(f"已清理临时目录: {args.temp_dir}")
    
    print(f"\n处理完成！成功转换 {success_count}/{len(p3_files)} 个文件")

if __name__ == "__main__":
    main() 