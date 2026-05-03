#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wavedb/appender.h"
#include "wavedb/database.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

class Catalog;  // 内部类型，用户不可见

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
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Status CreateTable(const TableSchema& schema);

    Status Insert(std::string_view table_name, const std::vector<Value>& row);

    Result<QueryResult> Select(
        std::string_view table_name,
        const std::vector<std::string>& columns = {"*"},
        Timestamp from_ts = 0,
        Timestamp to_ts = 0,
        int64_t limit = 0);

    Result<Appender> CreateAppender(std::string_view table_name);

    WaveDB& db();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wavedb
