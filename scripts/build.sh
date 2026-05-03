#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "=== 1/3 编译 wavedb 库 ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

echo "=== 2/3 安装到 ./lib ==="
cmake --install build --prefix .

echo "=== 3/3 编译 tools ==="

mkdir -p bin

for tool in writer reader; do
  echo "  → $tool"
  cd "tools/$tool"
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  cp build/$tool "$ROOT/bin/"
  cd "$ROOT"
done

echo ""
echo "=== 完成 ==="
echo "库文件:  ./lib/libwavedb.a  ./lib/libwavedb.so"
echo "可执行:  $(ls bin/)"
