import os
from pydub import AudioSegment

def convert_mp3_to_standard(input_directory):
    # 遍历目录中的所有文件
    for filename in os.listdir(input_directory):
        if filename.endswith(".mp3"):
            mp3_file_path = os.path.join(input_directory, filename)
            output_file_path = os.path.join(input_directory, f"converted_{filename}")

            # 加载 MP3 文件
            audio = AudioSegment.from_mp3(mp3_file_path)

            # 转换为 16 kHz 和 16-bit PCM
            audio = audio.set_frame_rate(16000).set_sample_width(2)  # 2 bytes = 16 bits

            # 导出为 MP3 文件
            audio.export(output_file_path, format="mp3", bitrate="128k")  # 设置比特率为 192 kbps
            print(f"Converted: {mp3_file_path} to {output_file_path}")

if __name__ == "__main__":
    input_directory = input("Enter the directory containing MP3 files: ")
    convert_mp3_to_standard(input_directory)