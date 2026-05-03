#include "src/engine/connection.h"

#include <span>

#include "src/storage/column_file.h"

namespace wavedb {

Connection::Connection(WaveDB& db) : db_(db) {
    // 延迟加载 catalog：尝试打开已有数据，若不存在则自动创建目录。
    auto cat = Catalog::Open(db_.path());
    if (cat.ok()) {
        catalog_ = std::move(*cat);
    }
}

Status Connection::CreateTable(const TableSchema& schema) {
    if (db_.read_only()) return Status(StatusCode::kInvalidArgument, "connection is read-only");
    return catalog_.CreateTable(schema);
}

Status Connection::Insert(std::string_view table_name, const std::vector<Value>& row) {
    if (db_.read_only()) return Status(StatusCode::kInvalidArgument, "connection is read-only");
    const TableSchema* schema = catalog_.GetTable(table_name);
    if (!schema) return Status(StatusCode::kNotFound, "table not found: " + std::string(table_name));

    if (row.size() != schema->column_count())
        return Status(
            StatusCode::kInvalidArgument, "column count mismatch: expected " + std::to_string(schema->column_count()) +
                                              ", got " + std::to_string(row.size()));

    // 打开列文件
    std::vector<ColumnFile> files;
    files.reserve(schema->column_count());
    for (size_t i = 0; i < schema->column_count(); ++i) {
        auto& col = schema->column_at(i);
        auto cf = ColumnFile::Open(ColPath(table_name, col.name), col.type);
        if (!cf.ok()) return cf.status;
        files.push_back(std::move(*cf));
    }

    // 逐列写一个值
    for (size_t i = 0; i < row.size(); ++i) {
        const auto& val = row[i];
        ColumnType type = schema->column_at(i).type;
        Status s;

        if (type == ColumnType::kFloat) {
            auto* d = std::get_if<double>(&val);
            if (!d)
                return Status(StatusCode::kInvalidArgument, "expected FLOAT for column " + schema->column_at(i).name);
            s = files[i].Append(std::span(d, 1));
        } else {
            auto* v = std::get_if<int64_t>(&val);
            if (!v)
                return Status(
                    StatusCode::kInvalidArgument, "expected INT/TIMESTAMP for column " + schema->column_at(i).name);
            s = files[i].Append(std::span(v, 1));
        }
        if (!s.ok()) return s;
    }

    return Status::OK();
}

Result<QueryResult> Connection::Select(
    std::string_view table_name,
    const std::vector<std::string>& columns,
    Timestamp from_ts,
    Timestamp to_ts) {
    const TableSchema* schema = catalog_.GetTable(table_name);
    if (!schema) return Status(StatusCode::kNotFound, "table not found: " + std::string(table_name));

    // 找到时间戳列（约定：第一个 kTimestamp 列）
    int ts_schema_idx = -1;
    for (size_t i = 0; i < schema->column_count(); ++i) {
        if (schema->column_at(i).type == ColumnType::kTimestamp) {
            ts_schema_idx = static_cast<int>(i);
            break;
        }
    }

    bool filter = (from_ts > 0 || to_ts > 0);
    if (filter && ts_schema_idx < 0)
        return Status(StatusCode::kInvalidArgument, "table has no TIMESTAMP column for range filter");

    bool select_all = columns.empty() || (columns.size() == 1 && columns[0] == "*");
    std::vector<int> col_indices;
    if (select_all) {
        for (size_t i = 0; i < schema->column_count(); ++i) col_indices.push_back(static_cast<int>(i));
    } else {
        for (const auto& name : columns) {
            int idx = schema->ColumnIndex(name);
            if (idx < 0) return Status(StatusCode::kNotFound, "column not found: " + name);
            col_indices.push_back(idx);
        }
    }

    // 判断 ts 列是否在选中列表中
    int ts_ci = -1;  // col_data 中的索引
    for (size_t ci = 0; ci < col_indices.size(); ++ci) {
        if (col_indices[ci] == ts_schema_idx) {
            ts_ci = static_cast<int>(ci);
            break;
        }
    }

    // 如果需要过滤但 ts 未选中，额外读取 ts 列数据
    std::vector<int64_t> ts_extra;
    if (filter && ts_ci < 0) {
        const auto& ts_col = schema->column_at(ts_schema_idx);
        auto cf = ColumnFile::Open(ColPath(table_name, ts_col.name), ts_col.type);
        if (!cf.ok()) return cf.status;
        auto data = cf->ReadAllInt64();
        if (!data.ok()) return data.status;
        ts_extra = std::move(*data);
    }

    // 按列读取
    std::vector<std::vector<Value>> col_data(col_indices.size());
    QueryResult result;

    for (size_t ci = 0; ci < col_indices.size(); ++ci) {
        int idx = col_indices[ci];
        const auto& col_def = schema->column_at(idx);

        result.column_names.push_back(col_def.name);
        result.column_types.push_back(col_def.type);
        result.column_precisions.push_back(col_def.precision);

        auto cf = ColumnFile::Open(ColPath(table_name, col_def.name), col_def.type);
        if (!cf.ok()) return cf.status;

        if (col_def.type == ColumnType::kFloat) {
            auto data = cf->ReadAllFloat64();
            if (!data.ok()) return data.status;
            for (double d : *data) col_data[ci].push_back(d);
        } else {
            auto data = cf->ReadAllInt64();
            if (!data.ok()) return data.status;
            for (int64_t v : *data) col_data[ci].push_back(v);
        }
    }

    // 列优先 → 行优先（带时间过滤）
    size_t nrows = col_data.empty() ? 0 : col_data[0].size();

    // 时间上界：to_ts == 0 表示不设上限
    Timestamp upper = (to_ts == 0) ? INT64_MAX : to_ts;

    result.rows.reserve(nrows);
    for (size_t r = 0; r < nrows; ++r) {
        // 时间范围过滤
        if (filter) {
            int64_t ts = (ts_ci >= 0) ? std::get<int64_t>(col_data[ts_ci][r]) : ts_extra[r];
            if (ts < from_ts || ts > upper) continue;
        }

        std::vector<Value> row;
        row.reserve(col_data.size());
        for (size_t c = 0; c < col_data.size(); ++c) row.push_back(col_data[c][r]);
        result.rows.push_back(std::move(row));
    }

    return result;
}

Result<Appender> Connection::CreateAppender(std::string_view table_name) {
    if (db_.read_only()) return Status(StatusCode::kInvalidArgument, "connection is read-only");
    const TableSchema* schema = catalog_.GetTable(table_name);
    if (!schema) return Status(StatusCode::kNotFound, "table not found: " + std::string(table_name));

    std::vector<ColumnFile> files;
    files.reserve(schema->column_count());
    for (size_t i = 0; i < schema->column_count(); ++i) {
        auto& col = schema->column_at(i);
        auto cf = ColumnFile::Open(ColPath(table_name, col.name), col.type);
        if (!cf.ok()) return cf.status;
        files.push_back(std::move(*cf));
    }

    return Appender(schema, std::move(files));
}

std::string Connection::ColPath(std::string_view table, std::string_view col) const {
    std::string p;
    p.reserve(catalog_.data_dir().size() + table.size() + col.size() + 16);
    p += catalog_.data_dir();
    p += '/';
    p += table;
    p += '/';
    p += col;
    p += ".col";
    return p;
}

}  // namespace wavedb
