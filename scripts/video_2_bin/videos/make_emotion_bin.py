import os, sys, subprocess, tempfile, shutil, struct
from PIL import Image

# 固定使用本机 ffmpeg 绝对路径，避免 ESP-IDF 终端找不到可执行文件
FFMPEG = r"C:\\Program Files\\FFmpeg\\ffmpeg-2025-10-27-git-68152978b5-full_build\\bin\\ffmpeg.exe"
if not os.path.isfile(FFMPEG):
    # 回退到环境变量中的 ffmpeg（如果用户已在当前终端加入了 PATH）
    FFMPEG = shutil.which("ffmpeg") or "ffmpeg"

TARGET_W, TARGET_H = 240, 240
# 目标单组大小（约）单位MB：将每个视频组限制在该容量内
TARGET_MB = 1.0
# 计算每帧大小与每组最大帧数
FRAME_SIZE = TARGET_W * TARGET_H * 2  # RGB565 bytes
MAX_FRAMES_PER_GROUP = max(1, int((TARGET_MB * 1024 * 1024) // FRAME_SIZE))

def extract_frames(video_path, out_dir, fps=30):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [FFMPEG, "-y", "-i", video_path,
           f"-vf", "fps=%d,scale=%d:%d:flags=lanczos" % (fps, TARGET_W, TARGET_H),
           os.path.join(out_dir, "%06d.png")]
    subprocess.check_call(cmd)

def rgb888_to_rgb565_le(img: Image.Image) -> bytes:
    img = img.convert("RGB")
    arr = img.tobytes()
    out = bytearray()
    for i in range(0, len(arr), 3):
        r,g,b = arr[i],arr[i+1],arr[i+2]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(rgb565 & 0xFF)
        out.append((rgb565 >> 8) & 0xFF)
    return bytes(out)

def pack_group_from_dir(frame_dir):
    files = sorted(f for f in os.listdir(frame_dir) if f.lower().endswith(".png"))
    # 限制每组帧数至 ~TARGET_MB 大小，必要时等间距抽样
    if len(files) > MAX_FRAMES_PER_GROUP:
        step = len(files) / MAX_FRAMES_PER_GROUP
        idxs = [int(i * step) for i in range(MAX_FRAMES_PER_GROUP)]
        # 去重并截断
        seen = set()
        sel = []
        for i in idxs:
            if i not in seen and i < len(files):
                sel.append(files[i])
                seen.add(i)
        files = sel
    frames = []
    for f in files:
        img = Image.open(os.path.join(frame_dir,f)).resize((TARGET_W,TARGET_H), Image.LANCZOS)
        frames.append(rgb888_to_rgb565_le(img))
    return frames

def main():
    if len(sys.argv) < 4:
        print("Usage: python make_emotion_bin.py output.bin fps video1.mp4 [video2.mp4 ...]")
        print(f"Each group will be limited to about {TARGET_MB} MB (~{MAX_FRAMES_PER_GROUP} frames at {TARGET_W}x{TARGET_H} RGB565)")
        sys.exit(1)
    out_bin = sys.argv[1]
    fps = int(sys.argv[2])
    videos = sys.argv[3:]
    temp_root = tempfile.mkdtemp(prefix="frames_")
    groups = []
    try:
        for idx, v in enumerate(videos):
            d = os.path.join(temp_root,f"group_{idx}")
            extract_frames(v,d,fps=fps)
            frames = pack_group_from_dir(d)
            if not frames: raise RuntimeError(f"No frames from {v}")
            groups.append(frames)
        with open(out_bin,"wb") as f:
            f.write(struct.pack("B", len(groups)))
            for frames in groups:
                f.write(struct.pack("<I", len(frames)))
            for frames in groups:
                for fb in frames:
                    f.write(fb)
        print("OK:", out_bin)
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)

if __name__=="__main__":
    main()