#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wavedb/schema.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 批量写入器。
//
// 设计核心：
//   缓冲时不持锁——AppendRow 仅将数据追加到内存 buffer（vector<vector<Value>>），
//   零系统调用、零 I/O。只在 Flush() / Close() 写盘时才获取 LOCK_EX，
//   写完后立即释放。这保证了高吞吐写入，同时锁持有时间最短。
//
// 缓冲布局：
//   buffers_[col_idx] 存储该列的所有缓冲行（列优先缓冲），
//   对应 Part::Create 的 columns 参数格式。
//
// 生命周期：
//   CreateAppender → AppendRow × N → Flush (可选多次) → Close
//   析构时若还有未刷盘数据则自动调用 WritePart。
//
// 时间戳追踪：
//   ts_col_idx_ 标识时间戳列（-1 表示无时间戳列）。
//   每次 AppendRow 更新 batch_min_ts_ / batch_max_ts_，
//   写入 Part 时写入 meta.json 供 PartManager 时间范围裁剪用。
//
// Part 拆分：
//   若 buffered_rows_ 超过 max_rows_per_part_，Flush 时自动拆分为多个 Part。
//   每个 Part 不超过 max_rows_per_part_ 行。
class Appender {
  public:
    Appender() = default;

    // schema 指针必须在 Appender 生命周期内有效（由 Connection 持有）。
    // ts_col_idx 为 TIMESTAMP 列的索引，-1 表示无时间戳列。
    // max_rows_per_part 控制单 Part 最大行数（0 视为 2048）。
    Appender(const TableSchema* schema, std::string table_dir, int ts_col_idx, int64_t max_rows_per_part = 2048);

    Appender(Appender&&) = default;
    Appender& operator=(Appender&&) = default;

    // 析构时自动刷盘未持久化的缓冲行。
    ~Appender();

    // 追加一行。校验列数、类型后加入缓冲区。
    Status AppendRow(const std::vector<Value>& row);

    // 变参便利方法：AppendRow(ts, price, volume) 等价于 AppendRow({ts, price, volume})。
    template <typename... Args>
    Status AppendRow(Args... args) {
        return AppendRow(std::vector<Value>{Value(args)...});
    }

    // 将缓冲行写入新的 Part 目录并清空缓冲区。
    Status Flush();

    // 等同于 Flush()。关闭后 Appender 仍可继续使用。
    Status Close();

    // 当前缓冲区中待刷盘的行数。
    size_t buffered_rows() const { return buffered_rows_; }
    // Appender 生命周期内累计写入的总行数。
    size_t total_rows() const { return total_rows_; }

  private:
    Status WritePart();

    const TableSchema* schema_ = nullptr;
    std::string table_dir_;
    int ts_col_idx_ = -1;  // TIMESTAMP 列索引，-1 表示无
    int64_t max_rows_per_part_ = 2048;  // 单 Part 最大行数

    // 列优先缓冲区：buffers_[col_idx][row_idx]
    std::vector<std::vector<Value>> buffers_;

    int64_t batch_min_ts_ = INT64_MAX;  // 当前批次最小时间戳
    int64_t batch_max_ts_ = 0;          // 当前批次最大时间戳
    size_t buffered_rows_ = 0;
    size_t total_rows_ = 0;
};

}  // namespace wavedb
