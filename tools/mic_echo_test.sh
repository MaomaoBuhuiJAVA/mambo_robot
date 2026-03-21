#!/usr/bin/env bash
# 回声测试：capture → 实时 pipe → playback，确认哪张卡是麦克风。
#
# 用法:
#   ./mic_echo_test.sh 3              # 只测 card 3（约 SEC 秒，默认 8）
#   ./mic_echo_test.sh                # 依次测 card 1 → 2 → 3
#
# 播放设备与 config.hpp 一致时可不改；否则指定:
#   PLAY=plughw:2,0 ./mic_echo_test.sh 3
#
# 依赖: arecord, aplay, timeout（coreutils）

set -euo pipefail
PLAY="${PLAY:-plughw:2,0}"
SEC="${SEC:-8}"

run_one() {
  local c="$1"
  echo ""
  echo "========== 录音 plughw:${c},0  ──实时──►  播放 ${PLAY}  (${SEC}s) =========="
  echo "对着麦克风说话，应从扬声器听到延迟回声。无声音则换 card 或检查 alsamixer。"
  sleep 1
  if ! command -v timeout >/dev/null 2>&1; then
    echo "未找到 timeout，改用 arecord -d（略有缓冲延迟）"
    arecord -D "plughw:${c},0" -f S16_LE -r 16000 -c 1 -t raw -d "$SEC" -q 2>/dev/null \
      | aplay -D "$PLAY" -f S16_LE -r 16000 -c 1 -q 2>/dev/null || true
  else
    timeout "${SEC}" arecord -D "plughw:${c},0" -f S16_LE -r 16000 -c 1 -t raw -q 2>/dev/null \
      | aplay -D "$PLAY" -f S16_LE -r 16000 -c 1 -q 2>/dev/null || true
  fi
  echo "--- 结束 card ${c} ---"
}

if [[ -n "${1:-}" ]]; then
  run_one "$1"
else
  for c in 1 2 3; do
    run_one "$c"
  done
fi

echo "全部完成。"
