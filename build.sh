#!/bin/bash
# 完整重新编译：清空 build 目录后 cmake + make，再启动（与 run.sh 行为一致）
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo ">>> 开始完整重新编译 Mambo Robot（会删除 build 目录）..."
rm -rf "$SCRIPT_DIR/build"
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"
cmake ..
make -j8

echo ">>> 编译完成，正在启动..."
exec bash "$SCRIPT_DIR/run.sh"
