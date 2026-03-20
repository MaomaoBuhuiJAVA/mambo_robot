#!/usr/bin/env python3
"""测试本地 LLM 连接"""
import urllib.request
import json

URLS = [
    "https://jhcyun.com/llm/v1/chat/completions",
    "https://124.222.205.168/llm/v1/chat/completions",
]

payload = json.dumps({
    "model": "qwen3.5-9b",
    "messages": [{"role": "user", "content": "你好，简单介绍一下你自己"}],
    "stream": False,
    "max_tokens": 50,
    "chat_template_kwargs": {"enable_thinking": False}
}).encode()

for url in URLS:
    print(f"\n>>> 测试: {url}")
    try:
        req = urllib.request.Request(
            url,
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        import ssl
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        with urllib.request.urlopen(req, context=ctx, timeout=15) as resp:
            body = json.loads(resp.read())
            reply = body["choices"][0]["message"]["content"]
            print(f"✓ 成功！回复: {reply}")
            break
    except Exception as e:
        print(f"✗ 失败: {e}")
