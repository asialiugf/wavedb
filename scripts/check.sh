#!/bin/bash
# 检查 WaveDB 数据目录中所有 Part 的 meta.json
# 用法: ./scripts/check.sh /tmp/kkk

set -e

DATA_DIR="${1:?Usage: $0 <data_dir>}"

for table_dir in "$DATA_DIR"/*/; do
    parts_dir="${table_dir}parts"
    [ -d "$parts_dir" ] || continue

    table_name=$(basename "$table_dir")
    echo "===== Table: $table_name ====="

    # 按名称排序所有 n_ 和 m_ 目录
    for part_dir in $(ls -1d "$parts_dir"/n_* "$parts_dir"/m_* 2>/dev/null | sort); do
        [ -d "$part_dir" ] || continue
        part_name=$(basename "$part_dir")
        meta="$part_dir/meta.json"
        echo ""
        echo "--- $part_name ---"
        if [ -f "$meta" ]; then
            cat "$meta"
        else
            echo "(no meta.json)"
        fi
    done
    echo ""
done
