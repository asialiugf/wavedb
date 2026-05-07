#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavedb/schema.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 不可变数据分区（仅管理 n_ 开头的普通 Part）。
//
// Part 是 WaveDB 的原子存储单元。每个 INSERT batch（Appender::Flush）
// 生成一个 n_YYYYMMDD_XXXXXX 格式的 Part 目录，包含：
//   meta.json              — 时间范围 + 行数
//   <column_name>.col × N  — 每列一个定长二进制文件
//
// Part 只管理 n_ 文件的生命周期（创建、命名、删除、截断）。
// m_ 开头的已合并 Part 由 Merge（PartManager）管理，Part 完全不知情。
//
// 不可变性保证：
//   Part 创建后内容永不修改。Reader 可安全并发访问已提交的 Part 而无需加锁。
//   合并部分消费后，通过 DiscardFirstRows 重写 .col 文件，
//   rename 保证原子性——Reader 始终看到一致的数据。
//
// 时间范围裁剪：
//   min_ts / max_ts 存储在 meta.json 中，供 PartManager 做粗粒度过滤。
//
// 列布局：
//   每列独立 .col 文件，裸二进制，无 header。行数通过文件大小/elem_size 推导。
//
// n_ 序号管理：
//   序号持久化在 <parts_dir>/.n_seq 文件中，单文件记录 "日期 序号"，
//   同天自增，跨天从 1 开始。

class Part {
  public:
    Part() = default;

    // ---- 工厂方法 ----

    // 创建新的 n_ Part。parts_dir 为基目录（如 table/parts），
    // 内部自动生成 n_YYYYMMDD_XXXXXX 完整路径并创建目录。
    // columns[col_idx][row_idx]：每个内层 vector 存储一列的全部行。
    static Result<Part> Create(
        std::string parts_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    // 以完整路径创建 Part（供 Merge 传已构造好的 m_ 路径使用）。
    static Result<Part> CreateWithPath(
        std::string part_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    // 以完整路径创建 Part，列文件使用块式压缩格式（v0.4+）。
    // TIMESTAMP → DoD, FLOAT/INT → NONE。
    static Result<Part> CreateBlocked(
        std::string part_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    // 打开已有 Part 目录，读取 meta.json 获取元信息。
    static Result<Part> Open(std::string part_dir, const TableSchema& schema);

    Part(Part&&) = default;
    Part& operator=(Part&&) = default;

    // ---- 属性访问 ----

    const std::string& dir() const { return dir_; }
    void set_dir(std::string d) { dir_ = std::move(d); }
    const TableSchema& schema() const { return schema_; }
    int64_t min_ts() const { return min_ts_; }
    int64_t max_ts() const { return max_ts_; }
    size_t row_count() const { return row_count_; }
    int64_t merge_boundary() const { return merge_boundary_; }
    void set_merge_boundary(int64_t b) { merge_boundary_ = b; }

    bool is_in_progress() const { return in_progress_; }
    void set_in_progress(bool v) { in_progress_ = v; }

    // 将当前状态持久化到 meta.json（in_progress / merge_boundary 变更后调用）
    Status PersistMeta() const;

    // 追加行列数据到已有 .col 文件末尾（渐进合并用）
    Status AppendColumns(const std::vector<std::vector<Value>>& columns);

    // 追加行列数据到已有 block 格式列文件末尾（渐进合并压缩路径用）。
    // 将 values 按 block_size 分块压缩后追加。
    Status AppendColumnsBlocked(const std::vector<std::vector<Value>>& columns);

    // ---- 列读写 ----

    // 读取指定列的全部数据，返回 Value 向量。
    Result<std::vector<Value>> ReadColumn(int col_idx, ColumnType type) const;

    // 从指定行偏移读取 count 行。
    // start + count 不得超过 row_count_。缺失列（ALTER TABLE ADD 后旧 Part）返回 count 个默认值。
    Result<std::vector<Value>> ReadColumnRange(int col_idx, ColumnType type, size_t start, size_t count) const;

    // 写入单列数据到该 Part。values 长度必须等于 row_count_。
    // 通过写入 .col.tmp 再 rename 保证原子性。
    Status WriteColumn(std::string_view col_name, ColumnType type, const std::vector<Value>& values) const;

    // ---- Merge 相关（仅 n_ Part 使用）----

    // 丢弃前 n 行：重新读取剩余行 → 覆盖写 .col → 更新 meta.json。
    // n 必须 < row_count_。由 PartManager::MergeParts 在部分消费时调用。
    Status DiscardFirstRows(size_t n);

    // ---- 删除 ----

    // 删除该 Part 的整个目录（仅 n_ Part 被完全合并后调用）。
    Status Delete() const;

    // ---- n_ 序号（静态工具方法）----

    // 获取下一个 n_ 序号。读 <parts_dir>/.n_seq → 同天自增，跨天重置。
    static int NextSeq(const std::string& parts_dir, int date);

    // 时间戳（微秒）→ YYYYMMDD 整数。
    static int TsToDate(int64_t ts_us);

  private:
    // 构造 Part 目录完整路径。
    static std::string MakePartDir(const std::string& parts_dir, int date, int seq);

    // 内部实现：创建 Part 目录并写入列文件 + meta.json。
    static Result<Part> CreateImpl(
        std::string part_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    std::string dir_;
    int64_t min_ts_ = 0;
    int64_t max_ts_ = 0;
    int64_t merge_boundary_ = 0;
    bool in_progress_ = false;
    size_t row_count_ = 0;
    mutable TableSchema schema_;
};

}  // namespace wavedb
