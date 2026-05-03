#include "src/engine/connection.h"

#include "src/storage/part_manager.h"

namespace wavedb {

Connection::Connection(WaveDB& db) : db_(db) {
  auto cat = Catalog::Open(db_.path());
  if (cat.ok()) catalog_ = std::move(*cat);
}

Status Connection::CreateTable(const TableSchema& schema) {
  if (db_.read_only())
    return Status(StatusCode::kInvalidArgument, "connection is read-only");
  return catalog_.CreateTable(schema);
}

// ---- Insert ----

Status Connection::Insert(std::string_view table_name,
                          const std::vector<Value>& row) {
  if (db_.read_only())
    return Status(StatusCode::kInvalidArgument, "connection is read-only");

  auto lock = FileLock::Acquire(db_.path(), /*exclusive=*/true);
  if (!lock.ok()) return lock.status;

  const TableSchema* schema = catalog_.GetTable(table_name);
  if (!schema)
    return Status(StatusCode::kNotFound,
                  "table not found: " + std::string(table_name));

  int ts_idx = -1;
  for (size_t i = 0; i < schema->column_count(); ++i)
    if (schema->column_at(i).type == ColumnType::kTimestamp)
      ts_idx = static_cast<int>(i);

  std::string table_dir = db_.path() + "/" + std::string(table_name);
  Appender appender(schema, std::move(table_dir), ts_idx, std::move(*lock));
  Status s = appender.AppendRow(row);
  if (!s.ok()) return s;
  return appender.Close();
}

// ---- CreateAppender ----

Result<Appender> Connection::CreateAppender(std::string_view table_name) {
  if (db_.read_only())
    return Status(StatusCode::kInvalidArgument, "connection is read-only");

  auto lock = FileLock::Acquire(db_.path(), /*exclusive=*/true);
  if (!lock.ok()) return lock.status;

  const TableSchema* schema = catalog_.GetTable(table_name);
  if (!schema)
    return Status(StatusCode::kNotFound,
                  "table not found: " + std::string(table_name));

  int ts_idx = -1;
  for (size_t i = 0; i < schema->column_count(); ++i) {
    if (schema->column_at(i).type == ColumnType::kTimestamp) {
      ts_idx = static_cast<int>(i);
      break;
    }
  }

  std::string table_dir = db_.path() + "/" + std::string(table_name);
  return Appender(schema, std::move(table_dir), ts_idx, std::move(*lock));
}

// ---- Select ----

Result<QueryResult> Connection::Select(
    std::string_view table_name, const std::vector<std::string>& columns,
    Timestamp from_ts, Timestamp to_ts) {
  // 读锁
  auto lock = FileLock::Acquire(db_.path(), /*exclusive=*/false);
  if (!lock.ok()) return lock.status;

  const TableSchema* schema = catalog_.GetTable(table_name);
  if (!schema)
    return Status(StatusCode::kNotFound,
                  "table not found: " + std::string(table_name));

  std::string table_dir = db_.path() + "/" + std::string(table_name);
  auto pm = PartManager::Open(table_dir, *schema);
  if (!pm.ok()) return pm.status;

  int ts_schema_idx = -1;
  for (size_t i = 0; i < schema->column_count(); ++i) {
    if (schema->column_at(i).type == ColumnType::kTimestamp) {
      ts_schema_idx = static_cast<int>(i);
      break;
    }
  }

  bool filter = (from_ts > 0 || to_ts > 0);
  Timestamp upper = (to_ts == 0) ? INT64_MAX : to_ts;

  auto parts = pm->GetPartsInRange(from_ts, to_ts);

  bool select_all =
      columns.empty() || (columns.size() == 1 && columns[0] == "*");
  std::vector<int> col_indices;
  if (select_all) {
    for (size_t i = 0; i < schema->column_count(); ++i)
      col_indices.push_back(static_cast<int>(i));
  } else {
    for (const auto& name : columns) {
      int idx = schema->ColumnIndex(name);
      if (idx < 0)
        return Status(StatusCode::kNotFound, "column not found: " + name);
      col_indices.push_back(idx);
    }
  }

  int ts_ci = -1;
  for (size_t ci = 0; ci < col_indices.size(); ++ci) {
    if (col_indices[ci] == ts_schema_idx) {
      ts_ci = static_cast<int>(ci);
      break;
    }
  }

  QueryResult result;
  for (size_t ci = 0; ci < col_indices.size(); ++ci) {
    const auto& col_def = schema->column_at(col_indices[ci]);
    result.column_names.push_back(col_def.name);
    result.column_types.push_back(col_def.type);
    result.column_precisions.push_back(col_def.precision);
  }

  std::vector<int64_t> ts_extra;
  if (filter && ts_ci < 0 && ts_schema_idx >= 0) {
    for (const auto* part : parts) {
      auto col = part->ReadColumn(ts_schema_idx, ColumnType::kTimestamp);
      if (!col.ok()) return col.status;
      for (const auto& v : *col) ts_extra.push_back(std::get<int64_t>(v));
    }
  }

  std::vector<std::vector<Value>> col_data(col_indices.size());
  for (const auto* part : parts) {
    for (size_t ci = 0; ci < col_indices.size(); ++ci) {
      const auto& col_def = schema->column_at(col_indices[ci]);
      auto col = part->ReadColumn(col_indices[ci], col_def.type);
      if (!col.ok()) return col.status;
      for (auto& v : *col) col_data[ci].push_back(std::move(v));
    }
  }

  size_t nrows = col_data.empty() ? 0 : col_data[0].size();
  result.rows.reserve(nrows);

  for (size_t r = 0; r < nrows; ++r) {
    if (filter && ts_schema_idx >= 0) {
      int64_t ts =
          (ts_ci >= 0) ? std::get<int64_t>(col_data[ts_ci][r]) : ts_extra[r];
      if (ts < from_ts || ts > upper) continue;
    }
    std::vector<Value> row;
    row.reserve(col_data.size());
    for (size_t c = 0; c < col_data.size(); ++c)
      row.push_back(col_data[c][r]);
    result.rows.push_back(std::move(row));
  }

  return result;
}

}  // namespace wavedb
