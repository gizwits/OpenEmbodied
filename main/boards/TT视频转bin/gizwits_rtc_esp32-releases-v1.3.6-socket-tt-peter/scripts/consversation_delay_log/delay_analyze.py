import re
from datetime import datetime

def analyze_log(file_path):
    czid_times = {}
    
    czid_start_times = {}
    playback_duration_start_times = {}
    czid_playback_durations = {}
    czid_times_diff = {}

    try:
        with open(file_path, 'r', encoding='utf-8') as file:
            last_timestamps = {}

            for line in file:
                # 提取时间戳和CZID及其后面的字段
                timestamp_match = re.search(r'\.\.(\d+:\d+:\d+:\d+)', line)
                czid_match = re.search(r'CZID:(\d+)(.*)', line)

                if timestamp_match and czid_match:
                    timestamp_str = timestamp_match.group(1)
                    czid = czid_match.group(1)
                    czid_field = czid_match.group(2).strip()

                    # 忽略特定条件的行
                    if "conversation.audio.delta len:120" in czid_field:
                        continue

                    # 将时间戳转换为datetime对象
                    timestamp = datetime.strptime(timestamp_str, '%H:%M:%S:%f')
                    timestamp_ms = timestamp.hour * 3600000 + timestamp.minute * 60000 + timestamp.second * 1000 + timestamp.microsecond / 1000

                    # 保存当前时间戳为毫秒
                    czid_times[czid] = timestamp
                    

                    # 计算同个CZID下两个字段的时间差
                    if czid in last_timestamps:
                        time_diff = timestamp_ms - last_timestamps[czid]
                        if czid in czid_times_diff:
                            czid_times_diff[czid] = czid_times_diff[czid] + time_diff
                        else:
                            czid_times_diff[czid] = time_diff
                        if time_diff > 20:
                            print(f'\033[91mCZID: {int(czid):02d}, [{timestamp.strftime("%H:%M:%S:%f")}][d:{time_diff}ms][a:{czid_times_diff[czid]}ms{czid_field}]\033[0m')
                        else:
                            print(f'CZID: {int(czid):02d}, [{timestamp.strftime("%H:%M:%S:%f")}][d:{time_diff}ms {czid_field}]')
                    else:
                        czid_times_diff[czid] = 0
                        print(f'CZID: {int(czid):02d}, [{timestamp.strftime("%H:%M:%S:%f")}{czid_field}]')

                    # 更新last_timestamps
                    last_timestamps[czid] = timestamp_ms

                    # playback_duration的开始从speech_stoped事件开始
                    if "speech_stoped" in czid_field:
                        playback_duration_start_times[czid] = timestamp

                    # duration的开始从speech_started事件开始
                    if "speech_started" in czid_field:
                        czid_start_times[czid] = timestamp

                    # 计算2s play timeout, stopping playback与speech_stoped的时间差
                    if "Buffer reached threshold" in czid_field:
                        if czid in czid_start_times:
                            playback_duration = timestamp - czid_start_times[czid]
                            czid_playback_durations[czid] = playback_duration.total_seconds()
    except FileNotFoundError:
        print(f"文件未找到: {file_path}")
        return {}

    # 计算每个CZID的总时间
    czid_durations = {}
    for czid, end_time in czid_times.items():
        if czid in czid_start_times:
            start_time = czid_start_times[czid]
            duration = end_time - start_time
            czid_durations[czid] = duration.total_seconds()
        else:
            print(f"CZID {czid} 缺少开始时间，无法计算持续时间。")

    # 计算playback_duration的平均值
    total_playback_duration = sum(czid_playback_durations.values())
    average_playback_duration = total_playback_duration / len(czid_playback_durations) if czid_playback_durations else 0
    print(f'平均playback_duration: {average_playback_duration} seconds')

    # 打印每个CZID的详细信息
    for czid, duration in czid_durations.items():
        start_time = playback_duration_start_times[czid]
        end_time = czid_times[czid]
        playback_duration = czid_playback_durations.get(czid, "N/A")
        print(f'CZID: {czid}, Start Time: {start_time.time()}, End Time: {end_time.time()}, Total Duration: {duration} seconds, Playback Duration: {playback_duration} seconds')

    return czid_durations

# 示例用法
log_file_path = '你好可以给我讲个故事吗5.log'
czid_durations = analyze_log(log_file_path)
