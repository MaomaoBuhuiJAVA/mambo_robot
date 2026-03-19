#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo ">>> 开始编译 Mambo Robot..."
rm -rf "$SCRIPT_DIR/build"
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"
cmake ..
make -j8

echo ">>> 编译完成，正在启动..."
sudo chmod 777 /dev/ttyS7 2>/dev/null || true
export LD_LIBRARY_PATH="$SCRIPT_DIR/third_party:$LD_LIBRARY_PATH"
export DISPLAY=${DISPLAY:-:0}
cd "$SCRIPT_DIR"

# 清理旧实例
pkill -f "mambo_robot" 2>/dev/null || true
pkill -f "firefox-esr" 2>/dev/null || true
sleep 1

# Ctrl+C 时同时杀掉所有子进程
cleanup() {
    echo ""
    echo ">>> 正在停止..."
    kill $BACKEND_PID $BROWSER_PID 2>/dev/null
    sleep 1
    kill -9 $BACKEND_PID $BROWSER_PID 2>/dev/null
    exit 0
}
trap cleanup SIGINT SIGTERM

# 启动后端
./build/mambo_robot &
BACKEND_PID=$!

# 等后端启动
sleep 2

# 启动浏览器
firefox-esr --kiosk --no-remote --window-size=1920,1080 http://localhost:8080 &
BROWSER_PID=$!

echo ">>> 运行中 (Ctrl+C 停止)"
echo ">>> 后端 PID: $BACKEND_PID  浏览器 PID: $BROWSER_PID"

# 等待后端退出
wait $BACKEND_PID
cleanup
