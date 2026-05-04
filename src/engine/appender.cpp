// Appender 实现：内存缓冲 → 批量刷盘。
//
// 缓冲策略：
//   AppendRow 仅将数据追加到列优先缓冲区 buffers_[col][row]，
//   不做任何 I/O。类型校验在入队时完成（失败不损失已有缓冲数据）。
//   Flush/Close 时调用 WritePart：持锁 → 创建 Part → 释放锁。
//
// 为什么不在 AppendRow 时持锁：
//   时序数据写入通常是高频、微批量。若每次 AppendRow 都持锁写盘，
//   吞吐量会受 fsync 延迟限制。缓冲 N 行后一次写入，可将 I/O 次数
//   降低 N 倍，锁持有时间也更短（不阻塞 Reader）。
//
// Part ID 恢复：
//   ScanNextPartId 在构造时扫描已有 Part 目录，取最大编号+1。
//   这允许数据库重启后继续从上次位置追加。

#include "wavedb/appender.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <span>

#include "src/storage/part.h"
#include "wavedb/database.h"

namespace wavedb {

// 扫描 parts/ 目录，返回下一个可用 Part 编号。
static int ScanNextPartId(const std::string& parts_dir) {
    int max_id = 0;
    DIR* dp = ::opendir(parts_dir.c_str());
    if (!dp) return 1;  // parts 目录不存在 → 从 001 开始
    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        int id = 0;
        bool ok = true;
        for (int i = 0; entry->d_name[i]; ++i) {
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') {
                ok = false;
                break;
            }
            id = id * 10 + (entry->d_name[i] - '0');
        }
        if (ok && id > max_id) max_id = id;
    }
    ::closedir(dp);
    return max_id + 1;
}

Appender::Appender(const TableSchema* schema, std::string table_dir, int ts_col_idx)
    : schema_(schema), table_dir_(std::move(table_dir)), ts_col_idx_(ts_col_idx) {
    // 预分配每列的缓冲区
    buffers_.resize(schema_->column_count());
    next_part_id_ = ScanNextPartId(table_dir_ + "/parts");
}

Appender::~Appender() {
    // 析构时自动刷盘未持久化数据
    if (buffered_rows_ > 0) WritePart();
}

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

Status Appender::WritePart() {
    // 仅在写盘时持锁。
    // data_dir 是 table_dir 的父目录（为兼容共享锁目录结构）。
    size_t pos = table_dir_.rfind('/');
    std::string data_dir = (pos != std::string::npos) ? table_dir_.substr(0, pos) : ".";
    auto lock = FileLock::Acquire(data_dir, /*exclusive=*/true);
    if (!lock.ok()) return lock.status;

    std::string parts_dir = table_dir_ + "/parts";
    ::mkdir(parts_dir.c_str(), 0755);  // 幂等创建

    std::string part_dir = NextPartDir();
    int64_t min_ts = (batch_min_ts_ == INT64_MAX) ? 0 : batch_min_ts_;
    int64_t max_ts = batch_max_ts_;

    // 委托 Part 工厂写入列文件 + meta.json
    auto result = Part::Create(part_dir, *schema_, buffers_, min_ts, max_ts);
    if (!result.ok()) return result.status;

    // 写盘成功后清空缓冲区
    for (auto& buf : buffers_) buf.clear();
    buffered_rows_ = 0;
    batch_min_ts_ = INT64_MAX;
    batch_max_ts_ = 0;
    return Status::OK();
}

std::string Appender::NextPartDir() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", next_part_id_++);
    return table_dir_ + "/parts/" + buf;
}

}  // namespace wavedb
