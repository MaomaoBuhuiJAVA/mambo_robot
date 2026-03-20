import asyncio
import ssl
import sys
import wave
from pathlib import Path

try:
    import httpx
except ImportError:
    print("请先安装 httpx: pip install httpx")
    sys.exit(1)

# =================== 配置区 ===================
# 注意：不要带尾部 / ，避免后端 /tts -> /tts/ 的 307 重定向问题
TTS_URL = "https://jhcyun.com/tts/"

TEXT = "你好，我是评估对比音频格式，当前按 24kHz 进行播放测试。"
CHARACTER = "klee"

# 服务端返回格式：PCM 24kHz / 16bit / 单声道
PCM_SAMPLE_RATE = 24000
PCM_SAMPLE_WIDTH = 2   # 16bit = 2 bytes
PCM_CHANNELS = 1

OUTPUT_PCM = "tts_output.pcm"
OUTPUT_WAV = "tts_output.wav"
TIMEOUT = 30.0

# 如果是 HTTPS 且证书为自签名，可设为 False
VERIFY_SSL = False
# =============================================


def save_wav_from_pcm(pcm_bytes: bytes, wav_path: str) -> None:
    with wave.open(wav_path, "wb") as wf:
        wf.setnchannels(PCM_CHANNELS)
        wf.setsampwidth(PCM_SAMPLE_WIDTH)
        wf.setframerate(PCM_SAMPLE_RATE)
        wf.writeframes(pcm_bytes)


async def main() -> int:
    payload = {
        "text": TEXT,
        "character": CHARACTER,
    }

    print(f"[*] 目标地址: {TTS_URL}")
    print(f"[*] 测试文本: {TEXT}")
    print(f"[*] 使用角色: {CHARACTER}")
    print(f"[*] 期望格式: PCM {PCM_SAMPLE_RATE}Hz / {PCM_SAMPLE_WIDTH * 8}bit / 单声道")
    print("[*] 发送请求...")

    verify = VERIFY_SSL
    if TTS_URL.startswith("https") and not VERIFY_SSL:
        ssl_ctx = ssl.create_default_context()
        ssl_ctx.check_hostname = False
        ssl_ctx.verify_mode = ssl.CERT_NONE
        verify = ssl_ctx

    try:
        async with httpx.AsyncClient(
            timeout=TIMEOUT,
            verify=verify,
            follow_redirects=False,
        ) as client:
            resp = await client.post(
                TTS_URL,
                json=payload,
                headers={"Content-Type": "application/json"},
            )

        print(f"[*] HTTP 状态: {resp.status_code}")
        content_type = resp.headers.get("content-type", "")
        print(f"[*] Content-Type: {content_type or '未返回'}")

        if resp.status_code != 200:
            print(f"[-] 请求失败: HTTP {resp.status_code}")
            location = resp.headers.get("location")
            if location:
                print(f"[-] 重定向到: {location}")
            try:
                print(f"[-] 错误 JSON: {resp.json()}")
            except Exception:
                body = resp.text[:500] if resp.text else ""
                if body:
                    print(f"[-] 响应: {body}")
            return 1

        audio_bytes = resp.content
        if len(audio_bytes) < 100:
            print(f"[-] 数据异常: 仅 {len(audio_bytes)} 字节")
            return 1

        pcm_path = Path(OUTPUT_PCM)
        wav_path = Path(OUTPUT_WAV)

        pcm_path.write_bytes(audio_bytes)
        save_wav_from_pcm(audio_bytes, str(wav_path))

        duration_sec = len(audio_bytes) / (PCM_SAMPLE_RATE * PCM_SAMPLE_WIDTH * PCM_CHANNELS)

        print(f"[+] 成功！音频大小: {len(audio_bytes) / 1024:.1f} KB")
        print(f"[+] 估算时长: {duration_sec:.2f} 秒")
        print(f"[+] 已保存 PCM: {pcm_path.resolve()}")
        print(f"[+] 已生成 WAV: {wav_path.resolve()}")
        print("\n[!] 播放方式：")
        print(f"    ffplay -f s16le -ar {PCM_SAMPLE_RATE} -ac {PCM_CHANNELS} {pcm_path}")
        print(f"    ffplay {wav_path}")
        return 0

    except httpx.ConnectError as e:
        print(f"[-] 连接失败: {repr(e)}")
        return 1
    except httpx.ReadTimeout:
        print("[-] 请求超时，请适当增大 TIMEOUT")
        return 1
    except Exception as e:
        print(f"[-] 异常: {type(e).__name__}: {e}")
        return 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
