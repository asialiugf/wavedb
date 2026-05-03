#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/catalog/schema.h"
#include "src/common/status.h"
#include "src/common/types.h"

namespace wavedb {

// 不可变数据分区。每个 INSERT batch 生成一个 Part。
class Part {
  public:
    Part() = default;

    // 从列优先数据创建新 Part。columns[col_idx][row_idx]。
    static Result<Part> Create(
        std::string part_dir,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns,
        int64_t min_ts,
        int64_t max_ts);

    // 打开已有 Part 目录，读取 meta.json。
    static Result<Part> Open(std::string part_dir, const TableSchema& schema);

    Part(Part&&) = default;
    Part& operator=(Part&&) = default;

    const std::string& dir() const { return dir_; }
    int64_t min_ts() const { return min_ts_; }
    int64_t max_ts() const { return max_ts_; }
    size_t row_count() const { return row_count_; }

    // 读取指定列的全部数据。
    Result<std::vector<Value>> ReadColumn(int col_idx, ColumnType type) const;

  private:
    std::string dir_;
    int64_t min_ts_ = 0;
    int64_t max_ts_ = 0;
    size_t row_count_ = 0;
    mutable TableSchema schema_;
};

}  // namespace wavedb
