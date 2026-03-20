#!/usr/bin/env python3
"""
测试本地部署的 LLM / ASR / TTS 接口连通性
运行：python3 test_local_api.py
"""
import json, ssl, time, wave, struct, urllib.request, urllib.error

BASE = "https://jhcyun.com"
# 如果域名解析失败，改用 IP
# BASE = "https://124.222.205.168"

# 忽略自签证书
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE

def post_json(url, payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data,
          headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, context=CTX, timeout=15) as r:
        return r.read().decode()

# ─────────────────────────────────────────────────────────────
# 1. LLM 非流式
# ─────────────────────────────────────────────────────────────
def test_llm():
    print("\n=== [1] LLM 非流式 ===")
    payload = {
        "model": "qwen3.5-9b",
        "messages": [
            {"role": "system", "content":
                "你是星宝，孤独症儿童陪伴机器人。严格按格式输出：\n"
                "[情绪][移动][转向]\n正文\n"
                "情绪：[happy][calm][sad][angry][scared][excited]\n"
                "移动：[forward:0.3][back:0.3][stay]\n"
                "转向：[left:15][right:15][face]\n"
                "正文简洁，禁用反问句。"},
            {"role": "user", "content": "你好星宝！"}
        ],
        "stream": False,
        "max_tokens": 80,
        "temperature": 0.1,
        "chat_template_kwargs": {"enable_thinking": False}
    }
    try:
        raw = post_json(f"{BASE}/llm/v1/chat/completions", payload)
        resp = json.loads(raw)
        content = resp["choices"][0]["message"]["content"]
        print(f"[OK] 回复：{content}")
        return True
    except Exception as e:
        print(f"[FAIL] {e}")
        return False

# ─────────────────────────────────────────────────────────────
# 2. TTS
# ─────────────────────────────────────────────────────────────
def test_tts():
    print("\n=== [2] TTS ===")
    payload = {"text": "你好，我是星宝，很高兴认识你！"}
    try:
        data = json.dumps(payload).encode()
        req = urllib.request.Request(f"{BASE}/tts/", data=data,
              headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, context=CTX, timeout=15) as r:
            pcm = r.read()
        print(f"[OK] 收到 PCM 数据 {len(pcm)} 字节")
        # 保存为可播放的 WAV（24000Hz/16bit/单声道）
        with wave.open("tts_test.wav", "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(24000)
            wf.writeframes(pcm)
        print("[OK] 已保存 tts_test.wav，用 aplay tts_test.wav 播放验证")
        return True
    except Exception as e:
        print(f"[FAIL] {e}")
        return False

# ─────────────────────────────────────────────────────────────
# 3. ASR（生成一段静音 PCM 测试连接，不期望识别出内容）
# ─────────────────────────────────────────────────────────────
def test_asr():
    print("\n=== [3] ASR WebSocket ===")
    try:
        import websocket  # pip3 install websocket-client
    except ImportError:
        print("[SKIP] 需要 websocket-client：pip3 install websocket-client")
        return False

    result = {"done": False, "text": ""}

    def on_message(ws, msg):
        try:
            d = json.loads(msg)
            if d.get("type") == "result":
                result["text"] = d.get("text", "")
                result["done"] = True
                ws.close()
        except Exception:
            pass

    def on_error(ws, err):
        print(f"[FAIL] WebSocket 错误: {err}")
        result["done"] = True

    def on_open(ws):
        # 发送 0.5 秒静音 PCM（16kHz/16bit/单声道）
        silence = struct.pack("<" + "h" * 8000, *([0] * 8000))
        ws.send(silence, opcode=websocket.ABNF.OPCODE_BINARY)
        ws.send(json.dumps({"type": "end"}))

    ws_url = BASE.replace("https://", "wss://").replace("http://", "ws://") + "/asr/"
    ws = websocket.WebSocketApp(ws_url,
         on_open=on_open, on_message=on_message, on_error=on_error)
    ws.run_forever(sslopt={"cert_reqs": ssl.CERT_NONE}, ping_timeout=10)

    if result["done"]:
        print(f"[OK] ASR 连接成功，识别结果：'{result['text']}'（静音正常为空）")
        return True
    else:
        print("[FAIL] ASR 未收到响应")
        return False

# ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"测试目标：{BASE}")
    r1 = test_llm()
    r2 = test_tts()
    r3 = test_asr()
    print("\n=== 结果汇总 ===")
    print(f"  LLM : {'✓' if r1 else '✗'}")
    print(f"  TTS : {'✓' if r2 else '✗'}")
    print(f"  ASR : {'✓' if r3 else '✗'}")
