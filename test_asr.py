import asyncio
import json
import ssl
import websockets

WS_URL = "wss://jhcyun.com/asr/"
PCM_FILE = "test.pcm"   # 16k/16bit/mono PCM 更稳

async def main():
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE

    async with websockets.connect(
        WS_URL,
        ssl=ssl_ctx,
        max_size=None,
        ping_interval=None,
    ) as ws:
        init_msg = {
            "mode": "2pass",
            "wav_name": "test",
            "is_speaking": True,
            "wav_format": "pcm",
            "chunk_size": [5, 10, 5],
            "itn": True,
            "audio_fs": 16000
        }
        await ws.send(json.dumps(init_msg, ensure_ascii=False))

        with open(PCM_FILE, "rb") as f:
            data = f.read()

        # 直接一次发完，做连通性测试够用
        await ws.send(data)

        # 结束标志
        await ws.send(json.dumps({"is_speaking": False}))

        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=5)
                print(msg)
        except asyncio.TimeoutError:
            pass

asyncio.run(main())