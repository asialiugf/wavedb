#include "src/engine/appender.h"

#include <span>

namespace wavedb {

Appender::Appender(const TableSchema* schema, std::vector<ColumnFile> files)
    : schema_(schema), files_(std::move(files)) {}

Appender::~Appender() {
    // Close 会先 Flush 再 fclose，未显式 Close 也能保证数据落盘。
    for (auto& f : files_) {
        if (f.row_count() > 0) f.Close();
    }
}

Status Appender::AppendRow(const std::vector<Value>& row) {
    if (row.size() != schema_->column_count()) return Status(StatusCode::kInvalidArgument, "column count mismatch");

    for (size_t i = 0; i < row.size(); ++i) {
        const auto& val = row[i];
        ColumnType type = schema_->column_at(i).type;
        Status s;

        if (type == ColumnType::kFloat) {
            auto* d = std::get_if<double>(&val);
            if (!d)
                return Status(StatusCode::kInvalidArgument, "expected FLOAT for column " + schema_->column_at(i).name);
            s = files_[i].Append(std::span(d, 1));
        } else {
            auto* v = std::get_if<int64_t>(&val);
            if (!v)
                return Status(
                    StatusCode::kInvalidArgument, "expected INT/TIMESTAMP for column " + schema_->column_at(i).name);
            s = files_[i].Append(std::span(v, 1));
        }
        if (!s.ok()) return s;
    }

    ++row_count_;
    return Status::OK();
}

Status Appender::Flush() {
    for (auto& f : files_) {
        Status s = f.Flush();
        if (!s.ok()) return s;
    }
    return Status::OK();
}

Status Appender::Close() {
    for (auto& f : files_) {
        Status s = f.Close();  // Close 内部会 Flush
        if (!s.ok()) return s;
    }
    files_.clear();
    return Status::OK();
}

}  // namespace wavedb
