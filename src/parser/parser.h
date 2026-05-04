// 极简 SQL 解析器（手写递归下降）。
//
// 支持的语法（v0.2）：
//   CREATE TABLE name (col TYPE [PRECISION], ...);
//   INSERT INTO name VALUES (val, ...);
//   SELECT [*|col,...] FROM name [WHERE ts >= val [AND ts <= val]] [LIMIT n];
//   ALTER TABLE name ADD COLUMN name TYPE;
//   ALTER TABLE name DROP COLUMN name;
//
// 类型: TIMESTAMP[(精度)], FLOAT, INT
// 精度: DAY, HOUR, MINUTE, SECOND, MILLI, MICRO（默认 MICRO）
//
// 设计：无 AST——解析时直接通过回调执行。
// 简单可维护，无需复杂中间表示。

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 解析器回调接口。解析到语义动作时调用，由 CLI 提供 Connection 执行。
struct ParseCallbacks {
    // CREATE TABLE name (col type, ...) [MERGE BY policy [MAX_ROWS n]]
    std::function<Status(
        std::string_view name,
        const std::vector<std::string>& col_names,
        const std::vector<ColumnType>& col_types,
        const std::vector<TimePrecision>& col_precs,
        MergeConfig merge_config)>
        on_create_table;

    // INSERT INTO name VALUES (val, ...)
    std::function<Status(std::string_view name, const std::vector<Value>& values)> on_insert;

    // SELECT [cols] FROM name [WHERE ts>=from [AND ts<=to]] [LIMIT n]
    // from_ts=0, to_ts=0, limit=0 表示该子句未出现
    std::function<Status(
        std::string_view name,
        const std::vector<std::string>& cols,
        Timestamp from_ts,
        Timestamp to_ts,
        int64_t limit,
        std::vector<std::string>& out_col_names,
        std::vector<ColumnType>& out_col_types,
        std::vector<TimePrecision>& out_col_precs,
        std::vector<std::vector<Value>>& out_rows)>
        on_select;

    // ALTER TABLE name ADD COLUMN name TYPE
    std::function<Status(std::string_view table, std::string_view col_name, ColumnType type, TimePrecision prec)>
        on_add_column;

    // ALTER TABLE name DROP COLUMN name
    std::function<Status(std::string_view table, std::string_view col_name)> on_drop_column;

    // UPDATE table SET col = val,... [FROM ts TO ts]
    // from_ts=0, to_ts=0 表示未指定范围（全表更新）
    std::function<Status(std::string_view table, std::string_view col_name, Timestamp from_ts, Timestamp to_ts,
                         const std::vector<Value>& values)>
        on_update_column;
};

// 解析一行 SQL 文本并调用相应回调。返回 Status。
// 不支持的语法返回 PARSE_ERROR。
Status ParseSQL(std::string_view sql, const ParseCallbacks& cb);

}  // namespace wavedb
