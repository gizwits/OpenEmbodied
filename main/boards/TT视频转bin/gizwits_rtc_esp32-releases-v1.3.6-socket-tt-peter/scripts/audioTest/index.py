from flask import Flask, request
import time
import os
import threading

app = Flask(__name__)

UPLOAD_FOLDER = "uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# 创建一个锁对象
write_lock = threading.Lock()

@app.route('/raw', methods=['POST'])
def upload_audio_raw():
    filename = f"{UPLOAD_FOLDER}/raw.bin"
    
    with write_lock:  # 在写入时获得锁
        with open(filename, "ab") as f:
            f.write(request.data)

    return "Chunk received", 200

@app.route('/aec', methods=['POST'])
def upload_audio_aec():
    filename = f"{UPLOAD_FOLDER}/aec.bin"
    
    with write_lock:  # 在写入时获得锁
        with open(filename, "ab") as f:
            f.write(request.data)

    return "Chunk received", 200

@app.route('/play', methods=['POST'])
def upload_audio_play():
    filename = f"{UPLOAD_FOLDER}/play.bin"
    
    with write_lock:  # 在写入时获得锁
        with open(filename, "ab") as f:
            f.write(request.data)

    return "Chunk received", 200

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000)