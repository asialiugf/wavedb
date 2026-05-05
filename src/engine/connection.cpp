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
#include "src/parser/parser.h"
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

// ---- QueryResult ----

struct QueryResult::Impl {
    TableSchema schema;                  // 表结构副本
    std::vector<Part> parts;             // 候选 Part（移动所有权）
    std::vector<int> col_indices;        // 投影列在 schema 中的索引
    std::vector<ColumnType> col_types;   // 投影列类型
    size_t part_idx = 0;                 // 当前 Part 索引
    size_t row_offset = 0;               // 当前 Part 内的行偏移
    size_t chunk_size = 2048;            // 每次 Fetch() 最大行数
    bool materialized = false;           // 是否已全量物化到 rows
};

QueryResult::~QueryResult() = default;

void QueryResult::SetChunkSize(size_t n) {
    if (impl_) impl_->chunk_size = n;
}

size_t QueryResult::RowCount() const {
    if (impl_ && !impl_->materialized) MaterializeRows();
    return rows.size();
}

void QueryResult::MaterializeRows() const {
    if (!impl_ || impl_->materialized) return;
    // 循环 Fetch → 转置行优先 → 填入 rows
    while (true) {
        auto ch = Fetch();
        if (!ch.ok() || ch->row_count == 0) break;
        for (size_t r = 0; r < ch->row_count; ++r) {
            std::vector<Value> row;
            row.reserve(ch->ColumnCount());
            for (size_t c = 0; c < ch->ColumnCount(); ++c) {
                if (ch->column_types[c] == ColumnType::FLOAT)
                    row.push_back(ch->columns[c].f64[r]);
                else
                    row.push_back(ch->columns[c].i64[r]);
            }
            rows.push_back(std::move(row));
        }
    }
    impl_->materialized = true;
}

Result<Chunk> QueryResult::Fetch() const {
    // DDL/DML 或无 impl：返回空 Chunk
    if (!impl_) {
        Chunk empty;
        empty.column_names = column_names;
        empty.column_types = column_types;
        empty.column_precisions = column_precisions;
        empty.columns.resize(ColumnCount());
        return empty;
    }

    // 跳过已读完的 Part（用逻辑行数 effective_row_count，Merge 消费后的 Part 实际可见行更少）
    while (impl_->part_idx < impl_->parts.size() &&
           impl_->row_offset >= impl_->parts[impl_->part_idx].effective_row_count()) {
        ++impl_->part_idx;
        impl_->row_offset = 0;
    }
    if (impl_->part_idx >= impl_->parts.size()) {
        // 无更多数据
        Chunk empty;
        for (size_t ci = 0; ci < impl_->col_indices.size(); ++ci) {
            const auto& col_def = impl_->schema.column_at(impl_->col_indices[ci]);
            empty.column_names.push_back(col_def.name);
            empty.column_types.push_back(col_def.type);
            empty.column_precisions.push_back(col_def.precision);
        }
        empty.columns.resize(empty.ColumnCount());
        return empty;
    }

    const auto& part = impl_->parts[impl_->part_idx];
    size_t nrows = std::min(impl_->chunk_size, part.effective_row_count() - impl_->row_offset);
    size_t ncols = impl_->col_indices.size();

    Chunk chunk;
    chunk.row_count = nrows;
    chunk.columns.resize(ncols);
    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = impl_->schema.column_at(impl_->col_indices[ci]);
        chunk.column_names.push_back(col_def.name);
        chunk.column_types.push_back(col_def.type);
        chunk.column_precisions.push_back(col_def.precision);
    }

    // 从磁盘逐列读取本块数据
    for (size_t ci = 0; ci < ncols; ++ci) {
        int schema_idx = impl_->col_indices[ci];
        ColumnType ctype = impl_->col_types[ci];

        auto col = part.ReadColumnRange(schema_idx, ctype, impl_->row_offset, nrows);
        if (!col.ok()) return col.status;

        ColumnChunk& cc = chunk.columns[ci];
        cc.type = ctype;
        if (ctype == ColumnType::FLOAT) {
            cc.f64.reserve(nrows);
            for (auto& v : *col) cc.f64.push_back(std::get<double>(v));
        } else {
            cc.i64.reserve(nrows);
            for (auto& v : *col) cc.i64.push_back(std::get<int64_t>(v));
        }
    }

    impl_->row_offset += nrows;
    return chunk;
}

Result<QueryResult> Connection::Query(std::string_view sql) {
    QueryResult result;

    ParseCallbacks cb;

    cb.on_create_table = [this, &result](
                             std::string_view name, const std::vector<std::string>& col_names,
                             const std::vector<ColumnType>& col_types,
                             const std::vector<TimePrecision>& col_precs,
                             MergeConfig merge_cfg) -> Status {
        TableSchema schema{std::string(name)};
        for (size_t i = 0; i < col_names.size(); ++i) schema.AddColumn(col_names[i], col_types[i], col_precs[i]);
        schema.setMergeConfig(merge_cfg);
        Status s = CreateTable(schema);
        if (s.ok()) {
            result.statement_type = StatementType::CREATE_TABLE;
            result.rows_affected = 0;
        }
        return s;
    };

    cb.on_insert = [this, &result](std::string_view name, const std::vector<Value>& values) -> Status {
        Status s = Insert(name, values);
        if (s.ok()) {
            result.statement_type = StatementType::INSERT;
            result.rows_affected = 1;
        }
        return s;
    };

    cb.on_select = [this, &result](
                       std::string_view name, const std::vector<std::string>& cols, Timestamp /*from_ts*/,
                       Timestamp /*to_ts*/, int64_t /*limit*/, std::vector<std::string>&, std::vector<ColumnType>&,
                       std::vector<TimePrecision>&, std::vector<std::vector<Value>>&) -> Status {
        const TableSchema* schema = impl_->catalog.GetTable(name);
        if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(name));

        std::string table_dir = impl_->db.path() + "/" + std::string(name);
        auto pm = PartManager::Open(table_dir, *schema);
        if (!pm.ok()) return pm.status;

        // 解析投影列
        bool select_all = cols.empty() || (cols.size() == 1 && cols[0] == "*");
        std::vector<int> col_indices;
        std::vector<ColumnType> col_types;
        if (select_all) {
            for (size_t i = 0; i < schema->column_count(); ++i) {
                col_indices.push_back(static_cast<int>(i));
                col_types.push_back(schema->column_at(i).type);
            }
        } else {
            for (const auto& cname : cols) {
                int idx = schema->ColumnIndex(cname);
                if (idx < 0) return Status(StatusCode::NOT_FOUND, "column not found: " + cname);
                col_indices.push_back(idx);
                col_types.push_back(schema->column_at(idx).type);
            }
        }

        // 填充结果元信息
        result.statement_type = StatementType::SELECT;
        for (size_t ci = 0; ci < col_indices.size(); ++ci) {
            const auto& col_def = schema->column_at(col_indices[ci]);
            result.column_names.push_back(col_def.name);
            result.column_types.push_back(col_def.type);
            result.column_precisions.push_back(col_def.precision);
        }
        result.rows_affected = 0;

        // 设置流式读取状态（惰性，不立即读数据）
        auto impl = std::make_unique<QueryResult::Impl>();
        impl->schema = *schema;
        impl->parts = pm->TakeParts();
        impl->col_indices = std::move(col_indices);
        impl->col_types = std::move(col_types);
        result.impl_ = std::move(impl);
        return Status::OK();
    };

    cb.on_add_column = [this, &result](std::string_view table, std::string_view col_name, ColumnType type,
                                       TimePrecision prec) -> Status {
        Status s = AddColumn(table, std::string(col_name), type, prec);
        if (s.ok()) {
            result.statement_type = StatementType::ALTER_ADD_COLUMN;
            result.rows_affected = 0;
        }
        return s;
    };

    cb.on_drop_column = [this, &result](std::string_view table, std::string_view col_name) -> Status {
        Status s = DropColumn(table, col_name);
        if (s.ok()) {
            result.statement_type = StatementType::ALTER_DROP_COLUMN;
            result.rows_affected = 0;
        }
        return s;
    };

    cb.on_update_column = [this, &result](std::string_view table, std::string_view col_name, Timestamp from_ts,
                                          Timestamp to_ts, const std::vector<Value>& values) -> Status {
        Status s;
        if (from_ts > 0 || to_ts > 0) {
            s = UpdateColumn(table, col_name, from_ts, to_ts, values);
        } else {
            s = UpdateColumn(table, col_name, values);
        }
        if (s.ok()) {
            result.statement_type = StatementType::UPDATE;
            result.rows_affected = static_cast<int64_t>(values.size());
        }
        return s;
    };

    Status s = ParseSQL(sql, cb);
    if (!s.ok()) return s;
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

    return DoUpdateColumn( *schema, col_name, col_type, parts, pm->total_rows(), values);
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

    return DoUpdateColumn( *schema, col_name, col_type, parts, total, values);
}

WaveDB& Connection::db() { return impl_->db; }

}  // namespace wavedb
