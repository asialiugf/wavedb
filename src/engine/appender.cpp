// Appender 实现：内存缓冲 → 批量刷盘。
//
// 缓冲策略：
//   AppendRow 仅将数据追加到列优先缓冲区 buffers_[col][row]，
//   不做任何 I/O。类型校验在入队时完成（失败不损失已有缓冲数据）。
//   Flush/Close 时调用 WritePart：构造 Part → 写入。
//
// 为什么不在 AppendRow 时持锁：
//   时序数据写入通常是高频、微批量。若每次 AppendRow 都持锁写盘，
//   吞吐量会受 fsync 延迟限制。缓冲 N 行后一次写入，可将 I/O 次数
//   降低 N 倍，锁持有时间也更短（不阻塞 Reader）。
//
// Part 拆分：
//   若 buffered_rows_ 超过 max_rows_per_part_，自动拆分为多个 n_ Part。
//   每个 Part 行数 ≤ max_rows_per_part_。拆分后的 Part 按时间顺序排列。

#include "wavedb/appender.h"

#include <sys/stat.h>

#include <cstdio>
#include <span>

#include "src/storage/part.h"
#include "src/storage/part_manager.h"

namespace wavedb {

Appender::Appender(const TableSchema* schema, std::string table_dir, int ts_col_idx, int64_t max_rows_per_part)
    : schema_(schema), table_dir_(std::move(table_dir)), ts_col_idx_(ts_col_idx), max_rows_per_part_(max_rows_per_part) {
    if (max_rows_per_part_ <= 0) max_rows_per_part_ = 2048;
    buffers_.resize(schema_->column_count());
}

Appender::~Appender() {
    // 析构时自动刷盘未持久化数据
    if (buffered_rows_ > 0) WritePart();
}

// 每次只追加一行，保持接口简单。批量写入由调用者控制 Flush 时机。
Status Appender::AppendRow(const std::vector<Value>& row) {
    // 列数校验
    if (row.size() != schema_->column_count()) return Status(StatusCode::INVALID_ARGUMENT, "column count mismatch");

    // 逐列类型校验（在缓冲前完成，保证缓冲区数据始终合法）
    for (size_t i = 0; i < row.size(); ++i) {
        const auto& val = row[i];
        ColumnType type = schema_->column_at(i).type;

        if (type == ColumnType::FLOAT && !std::holds_alternative<double>(val))
            return Status(StatusCode::INVALID_ARGUMENT, "expected FLOAT for column " + schema_->column_at(i).name);
        if (type != ColumnType::FLOAT && !std::holds_alternative<int64_t>(val))
            return Status(
                StatusCode::INVALID_ARGUMENT, "expected INT/TIMESTAMP for column " + schema_->column_at(i).name);
    }

    // 追踪批次时间范围
    if (ts_col_idx_ >= 0) {
        int64_t ts = std::get<int64_t>(row[ts_col_idx_]);
        if (ts < batch_min_ts_) batch_min_ts_ = ts;
        if (ts > batch_max_ts_) batch_max_ts_ = ts;
    }

    // 列优先追加
    for (size_t i = 0; i < row.size(); ++i) buffers_[i].push_back(row[i]);
    ++buffered_rows_;
    ++total_rows_;
    return Status::OK();
}

Status Appender::Flush() {
    if (buffered_rows_ == 0) return Status::OK();
    return WritePart();
}

Status Appender::Close() { return Flush(); }

// 按 max_rows_per_part_ 拆分 buffer 逐批写入 n_ Part。
// 每个 Part 的列数据切片 columns[offset..offset+take]，委托 Part::Create 自动命名。
Status Appender::WritePart() {
    std::string parts_dir = table_dir_ + "/parts";
    ::mkdir(parts_dir.c_str(), 0755);  // 幂等创建

    // 去重检查：写入的 TS 必须大于已有 Part 的最大 TS
    if (ts_col_idx_ >= 0 && buffered_rows_ > 0) {
        auto pm = PartManager::Open(table_dir_, *schema_);
        if (pm.ok()) {
            int64_t existing_max_ts = 0;
            for (auto& p : pm->all_parts()) {
                if (p.max_ts() > existing_max_ts) existing_max_ts = p.max_ts();
            }
            if (existing_max_ts > 0) {
                for (size_t r = 0; r < buffered_rows_; ++r) {
                    int64_t ts = std::get<int64_t>(buffers_[ts_col_idx_][r]);
                    if (ts <= existing_max_ts) {
                        // 检查失败，清空 buffer 避免死循环重试同一批旧数据
                        for (auto& buf : buffers_) buf.clear();
                        buffered_rows_ = 0;
                        batch_min_ts_ = INT64_MAX;
                        batch_max_ts_ = 0;
                        return Status(StatusCode::INVALID_ARGUMENT,
                            "TS " + std::to_string(ts) + " <= existing max_ts " + std::to_string(existing_max_ts) +
                            " — duplicate or out-of-order data not allowed");
                    }
                }
            }
        }
    }

    int64_t batch_min = (batch_min_ts_ == INT64_MAX) ? 0 : batch_min_ts_;
    int64_t batch_max = batch_max_ts_;
    size_t ncols = buffers_.size();
    size_t offset = 0;

    while (offset < buffered_rows_) {
        size_t take = static_cast<size_t>(max_rows_per_part_);
        if (offset + take > buffered_rows_) take = buffered_rows_ - offset;

        // 切片：取每列 [offset, offset+take) 的行
        std::vector<std::vector<Value>> sliced_cols(ncols);
        for (size_t ci = 0; ci < ncols; ++ci) {
            sliced_cols[ci].assign(
                buffers_[ci].begin() + static_cast<ptrdiff_t>(offset),
                buffers_[ci].begin() + static_cast<ptrdiff_t>(offset + take));
        }

        // 估算本批次的 min/max ts（如果有时序列）
        int64_t part_min = batch_min;
        int64_t part_max = batch_max;
        if (ts_col_idx_ >= 0 && take > 0) {
            part_min = std::get<int64_t>(sliced_cols[ts_col_idx_].front());
            part_max = std::get<int64_t>(sliced_cols[ts_col_idx_].back());
        }

        // 委托 Part 工厂：自动生成 n_YYYYMMDD_XXXXXX 路径 + 写入
        auto result = Part::Create(parts_dir, *schema_, sliced_cols, part_min, part_max);
        if (!result.ok()) return result.status;

        offset += take;
    }

    // 写盘成功后清空缓冲区
    for (auto& buf : buffers_) buf.clear();
    buffered_rows_ = 0;
    batch_min_ts_ = INT64_MAX;
    batch_max_ts_ = 0;
    return Status::OK();
}

}  // namespace wavedb
