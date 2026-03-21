#!/usr/bin/env bash
# 单条测试：从指定 ALSA 设备录 2 秒，打印与主程序一致的 mean(|sample|)「音量」
# 用法: ./tools/mic_level.sh [设备名]
# 例:   ./tools/mic_level.sh plughw:3,0

set -euo pipefail
DEV="${1:-plughw:3,0}"
echo "device=$DEV (speak into mic for 2s...)"
arecord -D "$DEV" -f S16_LE -r 16000 -c 1 -t raw -d 2 -q 2>/dev/null \
| python3 -c "
import sys, array
d = sys.stdin.buffer.read()
a = array.array('h')
a.frombytes(d[: len(d) // 2 * 2])
if not a:
    print('mean_abs: (no data — wrong device or capture muted?)')
    sys.exit(1)
v = sum(abs(x) for x in a) // len(a)
print('mean_abs:', v, '  (mambo uses this style in dialog_system; threshold in config.hpp)')
"
