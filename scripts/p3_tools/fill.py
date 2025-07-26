from pydub import AudioSegment

# 读取原始 bo.mp3
audio = AudioSegment.from_file("bo.mp3")

# 目标时长（毫秒）
target_duration_ms = 400

if len(audio) < target_duration_ms:
    # 计算需要补多少静音
    padding = AudioSegment.silent(duration=target_duration_ms - len(audio), frame_rate=audio.frame_rate)
    # 拼接
    audio = audio + padding
    print(f"音频已补足到 {target_duration_ms} ms")
else:
    print("音频长度已满足要求，无需补足")

# 保存为新文件
audio.export("bo_filled.mp3", format="mp3")
print("已保存为 bo_filled.mp3")