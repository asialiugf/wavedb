#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavedb/schema.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 批量写入器。缓冲时不持锁，仅在 Flush/Close 写盘时短暂获取 LOCK_EX。
class Appender {
  public:
    Appender() = default;

    Appender(const TableSchema* schema, std::string table_dir, int ts_col_idx);

    Appender(Appender&&) = default;
    Appender& operator=(Appender&&) = default;

    ~Appender();

    Status AppendRow(const std::vector<Value>& row);

    template <typename... Args>
    Status AppendRow(Args... args) {
        return AppendRow(std::vector<Value>{Value(args)...});
    }

    Status Flush();
    Status Close();

    size_t buffered_rows() const { return buffered_rows_; }
    size_t total_rows() const { return total_rows_; }

  private:
    Status WritePart();
    std::string NextPartDir() const;

    const TableSchema* schema_ = nullptr;
    std::string table_dir_;
    int ts_col_idx_ = -1;

    std::vector<std::vector<Value>> buffers_;

    int64_t batch_min_ts_ = INT64_MAX;
    int64_t batch_max_ts_ = 0;
    size_t buffered_rows_ = 0;
    size_t total_rows_ = 0;
    mutable int next_part_id_ = 1;
};

}  // namespace wavedb
