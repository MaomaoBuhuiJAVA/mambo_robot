#!/bin/bash
# 仅启动：不编译。需已存在 build/mambo_robot（先执行 ./build.sh）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$SCRIPT_DIR/build/mambo_robot"

if [[ ! -x "$BIN" ]]; then
    echo ">>> 未找到可执行文件: $BIN"
    echo ">>> 请先执行 ./build.sh 完成编译"
    exit 1
fi

export LD_LIBRARY_PATH="$SCRIPT_DIR/third_party:$LD_LIBRARY_PATH"
export DISPLAY=${DISPLAY:-:0}
cd "$SCRIPT_DIR"

echo ">>> 快速启动（跳过编译）..."

pkill -f "mambo_robot" 2>/dev/null || true
pkill -f "firefox-esr" 2>/dev/null || true
sleep 1

cleanup() {
    echo ""
    echo ">>> 正在停止..."
    kill $BACKEND_PID $BROWSER_PID 2>/dev/null
    sleep 1
    kill -9 $BACKEND_PID $BROWSER_PID 2>/dev/null
    exit 0
}
trap cleanup SIGINT SIGTERM

./build/mambo_robot &
BACKEND_PID=$!

sleep 2

firefox-esr --kiosk --no-remote --window-size=1920,1080 http://localhost:8080 &
BROWSER_PID=$!

echo ">>> 运行中 (Ctrl+C 停止)"
echo ">>> 后端 PID: $BACKEND_PID  浏览器 PID: $BROWSER_PID"

wait $BACKEND_PID
cleanup
