#!/usr/bin/env python3
"""
批量并行烧录ESP32设备的脚本
使用多线程同时烧录多个设备，提高效率
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from typing import List, Tuple

# 脚本所在目录
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# flash.sh 脚本路径
FLASH_SCRIPT = os.path.join(SCRIPT_DIR, "flash.sh")

def detect_ports() -> List[str]:
    """
    自动检测系统中的串口设备
    返回可能是ESP32设备的串口列表
    """
    ports = []
    
    # 检测不同操作系统的串口
    if sys.platform.startswith('linux'):
        # Linux系统，检查 /dev/ttyUSB* 和 /dev/ttyACM*
        import glob
        ports.extend(glob.glob('/dev/ttyUSB*'))
        ports.extend(glob.glob('/dev/ttyACM*'))
    elif sys.platform.startswith('win'):
        # Windows系统，使用 COM 端口
        try:
            import serial.tools.list_ports
            ports = [p.device for p in serial.tools.list_ports.comports()]
        except ImportError:
            print("警告: 未安装pyserial库，无法自动检测Windows串口")
            print("请安装: pip install pyserial")
            ports = [f"COM{i}" for i in range(1, 10)]  # 默认尝试COM1-COM9
    elif sys.platform.startswith('darwin'):
        # macOS系统
        import glob
        ports.extend(glob.glob('/dev/tty.usbmodem*'))
    
    return ports

def flash_device(port: str, extra_args: List[str] = None) -> Tuple[str, bool, str]:
    """
    烧录单个设备
    
    Args:
        port: 串口设备路径
        extra_args: 传递给flash.sh的额外参数
        
    Returns:
        Tuple[串口, 是否成功, 输出信息]
    """
    start_time = time.time()
    cmd = [FLASH_SCRIPT, "-p", port]
    
    if extra_args:
        cmd.extend(extra_args)
    
    print(f"开始烧录设备: {port}")
    try:
        result = subprocess.run(
            cmd,
            check=False,
            capture_output=True,
            text=True
        )
        success = result.returncode == 0
        output = result.stdout
        error = result.stderr
        
        elapsed = time.time() - start_time
        status = "成功" if success else "失败"
        print(f"设备 {port} 烧录{status}，耗时: {elapsed:.2f}秒")
        
        if not success:
            output += f"\n错误信息:\n{error}"
        
        return port, success, output
    except Exception as e:
        print(f"设备 {port} 烧录过程发生异常: {str(e)}")
        return port, False, str(e)

def main():
    parser = argparse.ArgumentParser(description="ESP32设备批量并行烧录工具")
    parser.add_argument("-p", "--ports", nargs="+", help="要烧录的串口列表，例如: -p /dev/ttyUSB0 /dev/ttyUSB1")
    parser.add_argument("-a", "--auto-detect", action="store_true", help="自动检测可用串口")
    parser.add_argument("-j", "--jobs", type=int, default=4, help="并行烧录的最大设备数量，默认为4")
    parser.add_argument("--extra-args", nargs=argparse.REMAINDER, help="传递给flash.sh的额外参数")
    
    args = parser.parse_args()
    
    # 获取串口列表
    ports = []
    if args.auto_detect:
        print("自动检测可用串口...")
        ports = detect_ports()
        if not ports:
            print("未检测到任何串口设备")
            return 1
        print(f"检测到以下串口设备: {', '.join(ports)}")
    elif args.ports:
        ports = args.ports
    else:
        parser.print_help()
        print("\n错误: 必须指定串口列表或使用自动检测")
        return 1
    
    # 检查flash.sh是否存在且可执行
    if not os.path.isfile(FLASH_SCRIPT):
        print(f"错误: 找不到烧录脚本 {FLASH_SCRIPT}")
        return 1
    
    if not os.access(FLASH_SCRIPT, os.X_OK):
        print(f"警告: 烧录脚本 {FLASH_SCRIPT} 不可执行，尝试添加执行权限")
        try:
            os.chmod(FLASH_SCRIPT, 0o755)
        except Exception as e:
            print(f"错误: 无法添加执行权限: {str(e)}")
            return 1
    
    # 开始并行烧录
    print(f"开始批量烧录 {len(ports)} 个设备，最大并行数: {args.jobs}")
    start_time = time.time()
    
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        future_to_port = {
            executor.submit(flash_device, port, args.extra_args): port 
            for port in ports
        }
        
        for future in concurrent.futures.as_completed(future_to_port):
            port = future_to_port[future]
            try:
                result = future.result()
                results.append(result)
            except Exception as e:
                print(f"设备 {port} 烧录任务异常: {str(e)}")
                results.append((port, False, str(e)))
    
    # 统计结果
    total_time = time.time() - start_time
    success_count = sum(1 for _, success, _ in results if success)
    
    print("\n批量烧录完成")
    print(f"总计: {len(ports)} 个设备")
    print(f"成功: {success_count} 个设备")
    print(f"失败: {len(ports) - success_count} 个设备")
    print(f"总耗时: {total_time:.2f}秒")
    
    # 输出失败的设备详情
    if len(ports) - success_count > 0:
        print("\n失败设备详情:")
        for port, success, output in results:
            if not success:
                print(f"\n设备: {port}")
                print("错误信息:")
                print(output)
    
    return 0 if success_count == len(ports) else 1

if __name__ == "__main__":
    sys.exit(main()) 