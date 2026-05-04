// Connection 实现：建表、写入、查询。
//
// 架构说明：
//   Connection 是门面（Facade）——将用户请求委托给：
//     Catalog   → 建表、表发现
//     Appender  → 批量写入、缓冲管理
//     PartManager → 查询时的 Part 加载和时间裁剪
//
// PIMPL 设计：
//   Connection::Impl 持有 Catalog，通过 unique_ptr 隐藏内部依赖。
//   用户只需 include connection.h 即可使用，不暴露内部头文件。
//
// Select 流程（v0.2 全量扫描）：
//   1. 从 Catalog 获取 TableSchema
//   2. PartManager::Open 加载所有 Part
//   3. GetPartsInRange 时间裁剪（粗过滤）
//   4. 候选 Part 逐列读取 → col_data
//   5. 逐行拼接 + 时间过滤（细过滤）+ limit 截断

#include "wavedb/connection.h"

#include "src/catalog/catalog.h"
#include "src/storage/part_manager.h"

namespace wavedb {

// Connection::Impl 在构造时加载 Catalog 快照。
// Catalog 加载失败不阻止 Connection 创建——表列表为空。
struct Connection::Impl {
    WaveDB& db;
    Catalog catalog;

    explicit Impl(WaveDB& db_ref) : db(db_ref) {
        auto cat = Catalog::Open(db.path());
        if (cat.ok()) catalog = std::move(*cat);
    }
};

Connection::Connection(WaveDB& db) : impl_(std::make_unique<Impl>(db)) {}

Connection::~Connection() = default;

Status Connection::CreateTable(const TableSchema& schema) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");
    return impl_->catalog.CreateTable(schema);
}

Status Connection::AddColumn(std::string_view table_name, std::string field_name, ColumnType type, TimePrecision prec) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");
    return impl_->catalog.AddColumn(table_name, std::move(field_name), type, prec);
}

Status Connection::DropColumn(std::string_view table_name, std::string_view field_name) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");
    return impl_->catalog.DropColumn(table_name, field_name);
}

Status Connection::Insert(std::string_view table_name, const std::vector<Value>& row) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");

    const TableSchema* schema = impl_->catalog.GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));

    // 查找 TIMESTAMP 列索引（用于 Appender 追踪 batch min/max ts）
    int ts_idx = -1;
    for (size_t i = 0; i < schema->column_count(); ++i)
        if (schema->column_at(i).type == ColumnType::TIMESTAMP) ts_idx = static_cast<int>(i);

    std::string table_dir = impl_->db.path() + "/" + std::string(table_name);
    Appender appender(schema, std::move(table_dir), ts_idx);
    Status s = appender.AppendRow(row);
    if (!s.ok()) return s;
    // 单行 Insert 直接 Close 刷盘
    return appender.Close();
}

Result<Appender> Connection::CreateAppender(std::string_view table_name) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");

    const TableSchema* schema = impl_->catalog.GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));

    int ts_idx = -1;
    for (size_t i = 0; i < schema->column_count(); ++i) {
        if (schema->column_at(i).type == ColumnType::TIMESTAMP) {
            ts_idx = static_cast<int>(i);
            break;  // 只取第一个 TIMESTAMP 列
        }
    }

    std::string table_dir = impl_->db.path() + "/" + std::string(table_name);
    return Appender(schema, std::move(table_dir), ts_idx);
}

Result<QueryResult> Connection::Select(
    std::string_view table_name,
    const std::vector<std::string>& columns,
    Timestamp from_ts,
    Timestamp to_ts,
    int64_t limit) {
    // Reader 不持锁——Part 不可变，已提交数据持久不变。
    const TableSchema* schema = impl_->catalog.GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));

    std::string table_dir = impl_->db.path() + "/" + std::string(table_name);
    auto pm = PartManager::Open(table_dir, *schema);
    if (!pm.ok()) return pm.status;

    // 定位 schema 中的 TIMESTAMP 列
    int ts_schema_idx = -1;
    for (size_t i = 0; i < schema->column_count(); ++i) {
        if (schema->column_at(i).type == ColumnType::TIMESTAMP) {
            ts_schema_idx = static_cast<int>(i);
            break;
        }
    }

    bool filter = (from_ts > 0 || to_ts > 0);
    Timestamp upper = (to_ts == 0) ? INT64_MAX : to_ts;

    // 步骤 1: Part 级时间裁剪
    auto parts = pm->GetPartsInRange(from_ts, to_ts);

    // 步骤 2: 解析投影列
    bool select_all = columns.empty() || (columns.size() == 1 && columns[0] == "*");
    std::vector<int> col_indices;
    if (select_all) {
        for (size_t i = 0; i < schema->column_count(); ++i) col_indices.push_back(static_cast<int>(i));
    } else {
        for (const auto& name : columns) {
            int idx = schema->ColumnIndex(name);
            if (idx < 0) return Status(StatusCode::NOT_FOUND, "column not found: " + name);
            col_indices.push_back(idx);
        }
    }

    // TIMESTAMP 列在选中列中的位置（用于行级过滤时直接索引 col_data）
    int ts_ci = -1;
    for (size_t ci = 0; ci < col_indices.size(); ++ci) {
        if (col_indices[ci] == ts_schema_idx) {
            ts_ci = static_cast<int>(ci);
            break;
        }
    }

    // 设置结果元信息
    QueryResult result;
    for (size_t ci = 0; ci < col_indices.size(); ++ci) {
        const auto& col_def = schema->column_at(col_indices[ci]);
        result.column_names.push_back(col_def.name);
        result.column_types.push_back(col_def.type);
        result.column_precisions.push_back(col_def.precision);
    }

    // 若需要时间过滤但 ts 未在投影列中，需单独读取 ts 列用于过滤判断
    std::vector<int64_t> ts_extra;
    if (filter && ts_ci < 0 && ts_schema_idx >= 0) {
        for (const auto* part : parts) {
            auto col = part->ReadColumn(ts_schema_idx, ColumnType::TIMESTAMP);
            if (!col.ok()) return col.status;
            for (const auto& v : *col) ts_extra.push_back(std::get<int64_t>(v));
        }
    }

    // 步骤 3: 逐 Part 逐列读取数据
    std::vector<std::vector<Value>> col_data(col_indices.size());
    for (const auto* part : parts) {
        for (size_t ci = 0; ci < col_indices.size(); ++ci) {
            const auto& col_def = schema->column_at(col_indices[ci]);
            auto col = part->ReadColumn(col_indices[ci], col_def.type);
            if (!col.ok()) return col.status;
            for (auto& v : *col) col_data[ci].push_back(std::move(v));
        }
    }

    // 步骤 4: 转行优先 + 行级时间过滤
    size_t nrows = col_data.empty() ? 0 : col_data[0].size();
    result.rows.reserve(nrows);

    for (size_t r = 0; r < nrows; ++r) {
        if (filter && ts_schema_idx >= 0) {
            int64_t ts = (ts_ci >= 0) ? std::get<int64_t>(col_data[ts_ci][r]) : ts_extra[r];
            if (ts < from_ts || ts > upper) continue;
        }
        std::vector<Value> row;
        row.reserve(col_data.size());
        for (size_t c = 0; c < col_data.size(); ++c) row.push_back(col_data[c][r]);
        result.rows.push_back(std::move(row));
    }

    // 步骤 5: limit 截断（取尾部）
    if (limit > 0 && result.rows.size() > static_cast<size_t>(limit)) {
        size_t start = result.rows.size() - static_cast<size_t>(limit);
        result.rows.erase(result.rows.begin(), result.rows.begin() + start);
    }

    return result;
}

std::vector<std::string> Connection::ListTables() const {
    std::vector<std::string> names;
    for (size_t i = 0; i < impl_->catalog.table_count(); ++i)
        names.push_back(impl_->catalog.GetTableByIndex(i)->name());
    return names;
}

const TableSchema* Connection::GetTableSchema(std::string_view name) const { return impl_->catalog.GetTable(name); }

// 内部：验证行数 → 持锁 → 逐 Part 写 .col
static Status DoUpdateColumn(
    WaveDB& db,
    const TableSchema& schema,
    std::string_view col_name,
    ColumnType col_type,
    const std::vector<const Part*>& parts,
    size_t total_rows,
    const std::vector<Value>& values) {
    if (values.size() != total_rows)
        return Status(
            StatusCode::INVALID_ARGUMENT,
            "values count mismatch: " + std::to_string(values.size()) + " vs " + std::to_string(total_rows) + " rows");

    auto lock = FileLock::Acquire(db.path(), /*exclusive=*/true);
    if (!lock.ok()) return lock.status;

    size_t offset = 0;
    for (auto* part : parts) {
        std::vector<Value> slice(values.begin() + offset, values.begin() + offset + part->row_count());
        Status s = part->WriteColumn(col_name, col_type, slice);
        if (!s.ok()) return s;
        offset += part->row_count();
    }
    return Status::OK();
}

Status
Connection::UpdateColumn(std::string_view table_name, std::string_view col_name, const std::vector<Value>& values) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");
    const TableSchema* schema = impl_->catalog.GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));
    int col_idx = schema->ColumnIndex(col_name);
    if (col_idx < 0) return Status(StatusCode::NOT_FOUND, "column not found: " + std::string(col_name));
    ColumnType col_type = schema->column_at(col_idx).type;

    std::string table_dir = impl_->db.path() + "/" + std::string(table_name);
    auto pm = PartManager::Open(table_dir, *schema);
    if (!pm.ok()) return pm.status;

    auto& all = pm->all_parts();
    if (all.empty()) return Status(StatusCode::NOT_FOUND, "no parts");
    std::vector<const Part*> parts;
    for (auto& p : all) parts.push_back(&p);

    return DoUpdateColumn(impl_->db, *schema, col_name, col_type, parts, pm->total_rows(), values);
}

Status Connection::UpdateColumn(
    std::string_view table_name,
    std::string_view col_name,
    Timestamp from_ts,
    Timestamp to_ts,
    const std::vector<Value>& values) {
    if (impl_->db.read_only()) return Status(StatusCode::INVALID_ARGUMENT, "connection is read-only");
    const TableSchema* schema = impl_->catalog.GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));
    int col_idx = schema->ColumnIndex(col_name);
    if (col_idx < 0) return Status(StatusCode::NOT_FOUND, "column not found: " + std::string(col_name));
    ColumnType col_type = schema->column_at(col_idx).type;

    std::string table_dir = impl_->db.path() + "/" + std::string(table_name);
    auto pm = PartManager::Open(table_dir, *schema);
    if (!pm.ok()) return pm.status;

    auto parts = pm->GetPartsInRange(from_ts, to_ts);
    if (parts.empty()) return Status(StatusCode::NOT_FOUND, "no parts in range");
    size_t total = 0;
    for (auto* p : parts) total += p->row_count();

    return DoUpdateColumn(impl_->db, *schema, col_name, col_type, parts, total, values);
}

WaveDB& Connection::db() { return impl_->db; }

}  // namespace wavedb
