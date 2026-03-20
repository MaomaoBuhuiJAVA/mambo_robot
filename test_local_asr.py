#!/usr/bin/env python3
"""测试本地 ASR WebSocket 连接，录5秒音频发送"""
import asyncio
import ssl
import json
import sys

try:
    import websockets
except ImportError:
    print("缺少 websockets 库，运行: pip3 install websockets")
    sys.exit(1)

try:
    import pyaudio
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False
    print("未找到 pyaudio，将使用静音测试数据")

ASR_URL = "wss://124.222.205.168/asr/"
SAMPLE_RATE = 16000
CHUNK = 1024
RECORD_SECONDS = 5

ssl_ctx = ssl.create_default_context()
ssl_ctx.check_hostname = False
ssl_ctx.verify_mode = ssl.CERT_NONE

async def test_asr(pcm_data: bytes):
    print(f">>> 连接 {ASR_URL} ...")
    async with websockets.connect(ASR_URL, ssl=ssl_ctx, max_size=None, ping_interval=None) as ws:
        print("✓ 连接成功")

        # 1. 发初始化消息
        init_msg = {
            "mode": "2pass",
            "wav_name": "test",
            "is_speaking": True,
            "wav_format": "pcm",
            "chunk_size": [5, 10, 5],
            "itn": True,
            "audio_fs": 16000
        }
        await ws.send(json.dumps(init_msg))
        print(">>> 已发送初始化消息")

        # 2. 发音频数据
        print(f">>> 发送 {len(pcm_data)} 字节 PCM...")
        chunk_size = SAMPLE_RATE * 2 // 10  # 100ms
        for i in range(0, len(pcm_data), chunk_size):
            await ws.send(pcm_data[i:i+chunk_size])
            await asyncio.sleep(0.05)

        # 3. 发结束标志
        await ws.send(json.dumps({"is_speaking": False}))
        print(">>> 已发送结束标志，等待识别结果...")

        # 4. 接收结果
        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=10)
                print(f"收到: {msg}")
        except asyncio.TimeoutError:
            print("✗ 超时")
        except Exception as e:
            print(f"连接关闭: {e}")

def record_audio():
    pa = pyaudio.PyAudio()
    stream = pa.open(format=pyaudio.paInt16, channels=1,
                     rate=SAMPLE_RATE, input=True, frames_per_buffer=CHUNK)
    print(f">>> 开始录音 {RECORD_SECONDS} 秒，请说话...")
    frames = [stream.read(CHUNK) for _ in range(int(SAMPLE_RATE / CHUNK * RECORD_SECONDS))]
    stream.stop_stream(); stream.close(); pa.terminate()
    print(">>> 录音结束")
    return b"".join(frames)

if __name__ == "__main__":
    if HAS_AUDIO:
        pcm = record_audio()
    else:
        with open("test.pcm", "rb") as f:
            pcm = f.read()
        print(f"使用 test.pcm ({len(pcm)} 字节)")
    asyncio.run(test_asr(pcm))
