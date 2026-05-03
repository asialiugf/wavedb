#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/catalog/schema.h"
#include "src/common/status.h"
#include "src/common/types.h"

namespace wavedb {

// 批量写入器。行数据先缓冲在内存中，Flush/Close 时写为一个 Part。
class Appender {
  public:
    Appender() = default;

    // schema: 表结构。table_dir: data/<db>/<table>/
    // ts_col_idx: 时间戳列索引（用于追踪 min/max ts）。
    Appender(const TableSchema* schema, std::string table_dir, int ts_col_idx);

    Appender(Appender&&) = default;
    Appender& operator=(Appender&&) = default;

    ~Appender();

    // 追加一行。values 数量必须等于列数，类型必须匹配。
    Status AppendRow(const std::vector<Value>& row);

    // variadic 便捷重载：AppendRow(ts, price, volume);
    template <typename... Args>
    Status AppendRow(Args... args) {
        return AppendRow(std::vector<Value>{Value(args)...});
    }

    // 将已缓冲的行写为一个 Part 并清空缓冲区。
    Status Flush();

    // 关闭 appender，将剩余行落盘。
    Status Close();

    size_t buffered_rows() const { return buffered_rows_; }
    size_t total_rows() const { return total_rows_; }

  private:
    Status WritePart();
    std::string NextPartDir() const;

    const TableSchema* schema_ = nullptr;
    std::string table_dir_;
    int ts_col_idx_ = -1;

    // 列优先缓冲：buffers_[col_idx][row_idx]
    std::vector<std::vector<Value>> buffers_;

    int64_t batch_min_ts_ = INT64_MAX;
    int64_t batch_max_ts_ = 0;
    size_t buffered_rows_ = 0;
    size_t total_rows_ = 0;
    mutable int next_part_id_ = 1;
};

}  // namespace wavedb
