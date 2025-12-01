#!/usr/bin/env python3
"""
将多个视频打包成一帧一帧的 JPEG 格式二进制文件

用法:
    python make_jpeg_bin.py output.bin fps quality video1.mp4 [video2.mp4 ...]

参数:
    output.bin: 输出的二进制文件名
    fps: 提取帧率（每秒帧数）
    quality: JPEG 质量 (1-100，数值越大质量越高，文件越大)
    video1.mp4 ...: 输入的视频文件列表

文件格式:
    [1 byte] 组数 (N)
    [N * 4 bytes] 每组的帧数 (uint32_t, little-endian)
    [N * 4 bytes] 每组的 JPEG 数据起始偏移量 (uint32_t, little-endian)
    [JPEG data] 所有组的 JPEG 帧数据（连续存储）
"""

import os
import sys
import subprocess
import tempfile
import shutil
import struct
from PIL import Image

# 固定使用本机 ffmpeg 绝对路径，避免 ESP-IDF 终端找不到可执行文件
FFMPEG = r"C:\\Program Files\\FFmpeg\\ffmpeg-2025-10-27-git-68152978b5-full_build\\bin\\ffmpeg.exe"
if not os.path.isfile(FFMPEG):
    # 回退到环境变量中的 ffmpeg（如果用户已在当前终端加入了 PATH）
    FFMPEG = shutil.which("ffmpeg") or "ffmpeg"

TARGET_W, TARGET_H = 240, 240

def extract_frames_to_jpeg(video_path, out_dir, fps=30, quality=85):
    """
    从视频提取帧并保存为 JPEG 格式
    
    Args:
        video_path: 输入视频路径
        out_dir: 输出目录
        fps: 提取帧率
        quality: JPEG 质量 (1-100)
    """
    os.makedirs(out_dir, exist_ok=True)
    cmd = [
        FFMPEG, "-y", "-i", video_path,
        "-vf", f"fps={fps},scale={TARGET_W}:{TARGET_H}:flags=lanczos",
        "-q:v", str(quality),  # JPEG 质量参数 (1-31, 越小质量越高，这里用 quality 映射)
        os.path.join(out_dir, "%06d.jpg")
    ]
    subprocess.check_call(cmd)

def pack_group_from_dir(frame_dir):
    """
    从目录中读取所有 JPEG 文件并打包
    
    Returns:
        list: JPEG 文件数据的列表
    """
    files = sorted(f for f in os.listdir(frame_dir) if f.lower().endswith(".jpg"))
    frames = []
    for f in files:
        jpeg_path = os.path.join(frame_dir, f)
        with open(jpeg_path, "rb") as img_file:
            jpeg_data = img_file.read()
            frames.append(jpeg_data)
    return frames

def main():
    if len(sys.argv) < 5:
        print("Usage: python make_jpeg_bin.py output.bin fps quality video1.mp4 [video2.mp4 ...]")
        print("Example: python make_jpeg_bin.py output.bin 30 85 video1.mp4 video2.mp4")
        sys.exit(1)
    
    out_bin = sys.argv[1]
    fps = int(sys.argv[2])
    quality = int(sys.argv[3])
    videos = sys.argv[4:]
    
    if quality < 1 or quality > 100:
        print("Error: quality must be between 1 and 100")
        sys.exit(1)
    
    # 将 quality (1-100) 转换为 ffmpeg 的 q:v 参数 (1-31, 越小质量越高)
    # 映射: 100 -> 2 (最高质量), 85 -> 5, 50 -> 15, 1 -> 31 (最低质量)
    # 公式: q:v = 2 + (100 - quality) * 29 / 99
    # 当 quality=100 时，q:v=2；当 quality=1 时，q:v=31
    ffmpeg_quality = int(2 + (100 - quality) * 29 / 99)
    if ffmpeg_quality < 1:
        ffmpeg_quality = 1
    if ffmpeg_quality > 31:
        ffmpeg_quality = 31
    
    print(f"Output file: {out_bin}")
    print(f"FPS: {fps}")
    print(f"JPEG quality: {quality} (ffmpeg q:v={ffmpeg_quality})")
    print(f"Target resolution: {TARGET_W}x{TARGET_H}")
    print(f"Processing {len(videos)} video(s)...")
    
    temp_root = tempfile.mkdtemp(prefix="jpeg_frames_")
    groups = []
    
    try:
        # 处理每个视频
        for idx, v in enumerate(videos):
            print(f"\nProcessing video {idx + 1}/{len(videos)}: {v}")
            d = os.path.join(temp_root, f"group_{idx}")
            extract_frames_to_jpeg(v, d, fps=fps, quality=ffmpeg_quality)
            frames = pack_group_from_dir(d)
            if not frames:
                raise RuntimeError(f"No frames extracted from {v}")
            groups.append(frames)
            print(f"  Extracted {len(frames)} frames")
        
        # 计算总大小
        total_frames = sum(len(g) for g in groups)
        total_jpeg_size = sum(sum(len(f) for f in g) for g in groups)
        print(f"\nTotal: {total_frames} frames, {total_jpeg_size / 1024 / 1024:.2f} MB")
        
        # 写入二进制文件
        print(f"\nWriting to {out_bin}...")
        with open(out_bin, "wb") as f:
            # 1. 写入组数
            f.write(struct.pack("B", len(groups)))
            
            # 2. 写入每组的帧数
            for frames in groups:
                f.write(struct.pack("<I", len(frames)))
            
            # 3. 计算并写入每组的 JPEG 数据起始偏移量
            # 偏移量从文件头之后开始计算
            header_size = 1 + len(groups) * 4 * 2  # 组数 + 帧数数组 + 偏移数组
            current_offset = header_size
            
            offsets = []
            for frames in groups:
                offsets.append(current_offset)
                # 计算当前组的总大小（每帧大小 + 4字节大小字段）
                group_size = sum(4 + len(frame) for frame in frames)
                current_offset += group_size
            
            for offset in offsets:
                f.write(struct.pack("<I", offset))
            
            # 4. 写入所有组的 JPEG 数据
            # 格式：每帧前先写入 4 字节的大小（uint32_t, little-endian），然后是 JPEG 数据
            for frames in groups:
                for jpeg_data in frames:
                    # 写入 JPEG 数据大小
                    f.write(struct.pack("<I", len(jpeg_data)))
                    # 写入 JPEG 数据
                    f.write(jpeg_data)
        
        # 获取输出文件大小
        output_size = os.path.getsize(out_bin)
        print(f"✓ Success! Output file: {out_bin} ({output_size / 1024 / 1024:.2f} MB)")
        
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)

if __name__ == "__main__":
    main()

