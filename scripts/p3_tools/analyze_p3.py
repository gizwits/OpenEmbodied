#!/usr/bin/env python3
# 分析p3格式文件的帧长和结构信息
import struct
import argparse
import os

def analyze_p3_file(input_file):
    """
    分析p3格式文件的结构信息
    p3格式: [1字节类型, 1字节保留, 2字节长度, Opus数据]
    """
    if not os.path.exists(input_file):
        print(f"错误: 文件 {input_file} 不存在")
        return
    
    file_size = os.path.getsize(input_file)
    print(f"文件: {input_file}")
    print(f"文件大小: {file_size} 字节")
    print("-" * 50)
    
    frame_count = 0
    total_payload_size = 0
    payload_sizes = []
    
    try:
        with open(input_file, 'rb') as f:
            offset = 0
            
            while True:
                # 记录当前位置
                frame_start = f.tell()
                
                # 读取头部 (4字节)
                header = f.read(4)
                if not header or len(header) < 4:
                    break
                
                # 解析头部
                packet_type, reserved, data_len = struct.unpack('>BBH', header)
                
                # 读取Opus数据
                opus_data = f.read(data_len)
                if not opus_data or len(opus_data) < data_len:
                    print(f"警告: 第 {frame_count + 1} 帧数据不完整")
                    break
                
                frame_count += 1
                total_payload_size += data_len
                payload_sizes.append(data_len)
                
                # 计算帧时长（基于采样率和采样点数估算）
                # 假设采样率为16000Hz，单声道
                sample_rate = 16000
                estimated_samples = data_len * 2.5  # Opus压缩比约为2.5:1
                estimated_duration_ms = (estimated_samples / sample_rate) * 1000
                
                print(f"帧 {frame_count:3d}: 类型={packet_type}, 保留={reserved}, "
                      f"长度={data_len:3d}字节, 估算时长={estimated_duration_ms:.1f}ms")
                
                # 移动到下一帧
                offset = f.tell()
                
    except Exception as e:
        print(f"解析错误: {e}")
        return
    
    if frame_count == 0:
        print("未找到有效的p3帧")
        return
    
    print("-" * 50)
    print(f"总帧数: {frame_count}")
    print(f"总负载大小: {total_payload_size} 字节")
    print(f"平均负载大小: {total_payload_size / frame_count:.1f} 字节")
    
    if payload_sizes:
        min_size = min(payload_sizes)
        max_size = max(payload_sizes)
        print(f"最小负载大小: {min_size} 字节")
        print(f"最大负载大小: {max_size} 字节")
        
        # 估算帧时长
        avg_payload_size = total_payload_size / frame_count
        estimated_samples = avg_payload_size * 2.5
        estimated_duration_ms = (estimated_samples / 16000) * 1000
        print(f"估算平均帧时长: {estimated_duration_ms:.1f}ms")
        
        # 分析帧时长分布
        print("\n帧时长分布分析:")
        if min_size == max_size:
            print("所有帧大小一致，可能是固定帧时长")
        else:
            print("帧大小不一致，可能是变长编码")
            
        # 统计不同大小的帧
        size_counts = {}
        for size in payload_sizes:
            size_counts[size] = size_counts.get(size, 0) + 1
        
        print("\n帧大小分布:")
        for size in sorted(size_counts.keys()):
            count = size_counts[size]
            percentage = (count / frame_count) * 100
            estimated_duration = (size * 2.5 / 16000) * 1000
            print(f"  {size:3d}字节 ({estimated_duration:.1f}ms): {count:3d}帧 ({percentage:5.1f}%)")

def main():
    parser = argparse.ArgumentParser(description='分析p3格式文件的结构信息')
    parser.add_argument('input_file', help='输入的p3文件路径')
    args = parser.parse_args()
    
    analyze_p3_file(args.input_file)

if __name__ == "__main__":
    main() 