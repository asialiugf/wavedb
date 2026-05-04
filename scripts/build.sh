#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# ---- 清理构建产物 ----
cmd_clear() {
    echo "=== 清理 ./bin ./lib ./build ==="
    for dir in bin lib build; do
        if [ -d "$ROOT/$dir" ]; then
            echo "  删除 $dir/"
            rm -rf "$ROOT/$dir"/*
        fi
    done
    echo "=== 清理完成 ==="
}

# ---- 编译并运行单元测试 ----
cmd_test() {
    echo "=== 编译单元测试 ==="
    cmake -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build

    echo ""
    echo "=== 运行单元测试 ==="
    failed=0

    for t in test_common test_catalog test_storage test_engine; do
        echo "--- $t ---"
        if [ -x "$ROOT/build/$t" ]; then
            "$ROOT/build/$t" || failed=1
        else
            echo "  跳过（$t 不存在）"
        fi
    done

    echo ""
    if [ "$failed" -eq 0 ]; then
        echo "=== 全部测试通过 ==="
    else
        echo "=== 存在失败测试 ==="
        exit 1
    fi
}

# ---- 默认：编译 + 安装 + tools ----
cmd_build() {
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
    if [ -d "$ROOT/bin" ] && [ "$(ls -A "$ROOT/bin" 2>/dev/null)" ]; then
        echo "可执行:  $(ls bin/)"
    fi
}

# ---- 帮助 ----
cmd_help() {
    echo "用法: build.sh [子命令]"
    echo ""
    echo "子命令:"
    echo "  (无)     编译库 + tools，安装到 ./lib ./bin"
    echo "  clear    删除 ./bin ./lib ./build 下所有文件"
    echo "  test     编译并运行全部单元测试"
    echo "  help     显示此帮助"
}

# ---- 入口 ----
case "${1:-}" in
    clear)
        cmd_clear
        ;;
    test)
        cmd_test
        ;;
    help|--help|-h)
        cmd_help
        ;;
    "")
        cmd_build
        ;;
    *)
        echo "未知子命令: $1"
        cmd_help
        exit 1
        ;;
esac
