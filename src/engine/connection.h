#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "src/catalog/catalog.h"
#include "src/common/status.h"
#include "src/common/types.h"
#include "src/engine/appender.h"
#include "src/engine/wavedb.h"

namespace wavedb {

struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<ColumnType> column_types;
    std::vector<TimePrecision> column_precisions;
    std::vector<std::vector<Value>> rows;  // 行优先

    size_t RowCount() const { return rows.size(); }
    size_t ColumnCount() const { return column_names.size(); }
};

class Connection {
  public:
    explicit Connection(WaveDB& db);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 建表。
    Status CreateTable(const TableSchema& schema);

    // 单行插入（便捷方法，批量请用 Appender）。
    Status Insert(std::string_view table_name, const std::vector<Value>& row);

    // 查询。columns 为空或 {"*"} 表示全列。
    // from_ts / to_ts 为微秒时间戳，0 表示不限制。
    Result<QueryResult> Select(
        std::string_view table_name,
        const std::vector<std::string>& columns = {"*"},
        Timestamp from_ts = 0,
        Timestamp to_ts = 0);

    // 创建批量写入 Appender。
    Result<Appender> CreateAppender(std::string_view table_name);

    const Catalog& catalog() const { return catalog_; }
    WaveDB& db() { return db_; }

  private:
    std::string ColPath(std::string_view table, std::string_view col) const;

    WaveDB& db_;
    Catalog catalog_;
};

}  // namespace wavedb
