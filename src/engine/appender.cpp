#include "src/engine/appender.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <span>

#include "src/storage/part.h"

namespace wavedb {

static int ScanNextPartId(const std::string& parts_dir) {
    int max_id = 0;
    DIR* dp = ::opendir(parts_dir.c_str());
    if (!dp) return 1;
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
    buffers_.resize(schema_->column_count());
    // 扫描已有 parts 确定下一个 ID
    next_part_id_ = ScanNextPartId(table_dir_ + "/parts");
}

Appender::~Appender() {
    if (buffered_rows_ > 0) WritePart();
}

Status Appender::AppendRow(const std::vector<Value>& row) {
    if (row.size() != schema_->column_count()) return Status(StatusCode::kInvalidArgument, "column count mismatch");

    for (size_t i = 0; i < row.size(); ++i) {
        const auto& val = row[i];
        ColumnType type = schema_->column_at(i).type;

        if (type == ColumnType::kFloat && !std::holds_alternative<double>(val))
            return Status(StatusCode::kInvalidArgument, "expected FLOAT for column " + schema_->column_at(i).name);
        if (type != ColumnType::kFloat && !std::holds_alternative<int64_t>(val))
            return Status(
                StatusCode::kInvalidArgument, "expected INT/TIMESTAMP for column " + schema_->column_at(i).name);
    }

    // 追踪 min/max ts
    if (ts_col_idx_ >= 0) {
        int64_t ts = std::get<int64_t>(row[ts_col_idx_]);
        if (ts < batch_min_ts_) batch_min_ts_ = ts;
        if (ts > batch_max_ts_) batch_max_ts_ = ts;
    }

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
    std::string parts_dir = table_dir_ + "/parts";
    ::mkdir(parts_dir.c_str(), 0755);

    std::string part_dir = NextPartDir();
    int64_t min_ts = (batch_min_ts_ == INT64_MAX) ? 0 : batch_min_ts_;
    int64_t max_ts = batch_max_ts_;

    auto result = Part::Create(part_dir, *schema_, buffers_, min_ts, max_ts);
    if (!result.ok()) return result.status;

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
