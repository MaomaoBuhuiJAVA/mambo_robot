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

# 关掉旧的 firefox-esr 实例
pkill -f "firefox-esr" 2>/dev/null || true
sleep 1

# 启动后端
./build/mambo_robot &
BACKEND_PID=$!

# 等后端启动
sleep 2

# 启动浏览器全屏显示眼睛UI（--kiosk 自动全屏，--window-size 消除白边）
firefox-esr --kiosk --no-remote --window-size=1920,1080 http://localhost:8080 &

# 等待后端退出
wait $BACKEND_PID
