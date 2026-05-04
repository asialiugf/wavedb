#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavedb/schema.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 不可变数据分区。
//
// Part 是 WaveDB 的原子存储单元。每个 INSERT batch（Appender::Flush）
// 生成一个 Part 目录，包含：
//   meta.json              — 时间范围 + 行数
//   <column_name>.col × N  — 每列一个定长二进制文件
//
// 不可变性保证：
//   Part 创建后内容永不修改。Reader 可安全并发访问已提交的 Part 而无需加锁。
//   这等价于 LSM 的 SST file 或 Parquet 的 row group 语义。
//
// 时间范围裁剪：
//   min_ts / max_ts 存储在 meta.json 中，供 PartManager 做粗粒度过滤，
//   避免扫描不相关 Part。但这是粗过滤——Part 内部的行级时间戳仍需逐行检查
//   （当多个时间戳落在同一 Part 且范围不精确时）。
//
// 列布局：
//   每列独立 .col 文件，裸二进制，无 header。行数通过文件大小/elem_size 推导。
//   列文件命名与 schema 列名一致——列重命名需要复制所有 Part 数据。

class Part {
  public:
    Part() = default;

    // 从列优先数据创建新 Part。
    // columns[col_idx][row_idx]：每个内层 vector 存储一列的全部行。
    // min_ts / max_ts 写入 meta.json。
    static Result<Part> Create(
        std::string part_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    // 打开已有 Part 目录，读取 meta.json 获取元信息。
    static Result<Part> Open(std::string part_dir, const TableSchema& schema);

    Part(Part&&) = default;
    Part& operator=(Part&&) = default;

    const std::string& dir() const { return dir_; }
    void set_dir(std::string d) { dir_ = std::move(d); }
    const TableSchema& schema() const { return schema_; }
    int64_t min_ts() const { return min_ts_; }
    int64_t max_ts() const { return max_ts_; }
    size_t row_count() const { return row_count_; }
    int64_t merge_boundary() const { return merge_boundary_; }
    void set_merge_boundary(int64_t b) { merge_boundary_ = b; }

    // 读取指定列的全部数据，返回 Value 向量。
    // col_idx 为 schema 中的列索引，type 必须匹配 schema 定义。
    Result<std::vector<Value>> ReadColumn(int col_idx, ColumnType type) const;

    // 从指定行偏移读取 count 行，返回 Value 向量。
    // start + count 不得超过 row_count_。缺失列（ALTER TABLE ADD 后旧 Part）返回 count 个默认值。
    Result<std::vector<Value>> ReadColumnRange(int col_idx, ColumnType type, size_t start, size_t count) const;

    // 写入单列数据到该 Part。values 长度必须等于 row_count_。
    // 通过写入 .col.tmp 再 rename 保证原子性。
    Status WriteColumn(std::string_view col_name, ColumnType type, const std::vector<Value>& values) const;

  private:
    std::string dir_;
    int64_t min_ts_ = 0;
    int64_t max_ts_ = 0;
    int64_t merge_boundary_ = 0;  // 合并窗口键，0 表示未设置
    size_t row_count_ = 0;
    mutable TableSchema schema_;  // mutable: ReadColumn 内部打开列文件需要类型信息
};

}  // namespace wavedb
