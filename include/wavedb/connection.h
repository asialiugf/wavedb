// 数据库连接与查询接口。
//
// Connection 是对用户暴露的主要操作接口，内部持有 Catalog
// 并将 CreateTable/Insert/Select/CreateAppender 委托给相应子系统。
//
// 线程安全：
//   当前每个 Connection 应单线程使用。多线程需各自创建 Connection。
//   所有 Connection 共享同一个 WaveDB 实例是安全的（WaveDB 不可变）。
//
// Select 设计：
//   当前是全量扫描语义（无索引），适合分析型查询。
//   读取路径不加锁——依赖 Part 的不可变性保证一致性。
//   (1) 已写入的 Part 不会被修改或删除
//   (2) 新 Part 追加对 Reader 可见性取决于 PartManager 的已加载快照
//
// QueryResult 使用行优先存储（vector<vector<Value>>），
// 方便遍历时按行访问。若未来需要列优先遍历，可加 ColData() 接口。

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

// 单元格值。隐式转换到 int64_t/double（Timestamp = int64_t），省去 std::get。
struct Cell {
    const Value& val;

    operator int64_t() const { return std::get<int64_t>(val); }
    operator double() const { return std::get<double>(val); }
};

// 单行视图。按列名访问值，轻量栈对象。
struct RowView {
    const std::vector<std::string>& col_names;
    const std::vector<Value>& row_data;

    // 按列名取值，返回可隐式转换的 Cell。O(n) 线性扫描（列数很小）。
    Cell operator[](std::string_view col_name) const {
        for (size_t i = 0; i < col_names.size(); ++i)
            if (col_names[i] == col_name) return {row_data[i]};
        static Value nil = int64_t(0);
        return {nil};
    }

    // 按索引取值
    Cell At(size_t i) const { return {row_data[i]}; }
};

// 列优先数据块中的单列数据。
// INT/TIMESTAMP 使用 i64 存储，FLOAT 使用 f64 存储，由 type 字段区分。
struct ColumnChunk {
    std::vector<int64_t> i64;  // INT 或 TIMESTAMP 数据
    std::vector<double> f64;   // FLOAT 数据
    ColumnType type = ColumnType::INT;

    size_t size() const { return type == ColumnType::FLOAT ? f64.size() : i64.size(); }
};

// 列优先数据块。每次 Fetch() 返回一个 Chunk。
// columns[col] 存储该列在 [row_offset, row_offset + row_count) 范围内的所有行数据。
struct Chunk {
    std::vector<std::string> column_names;          // 选中列的名称
    std::vector<ColumnType> column_types;           // 选中列的类型
    std::vector<TimePrecision> column_precisions;   // 选中列的精度（TIMESTAMP 用）
    std::vector<ColumnChunk> columns;               // 每列一个 ColumnChunk
    size_t row_count = 0;                           // 本块行数（所有列的 size() 应相等）

    size_t ColumnCount() const { return column_names.size(); }

    // 按索引访问列
    ColumnChunk& Column(size_t i) { return columns[i]; }
    // 按名称查找列索引，O(n)
    int ColumnIndex(std::string_view name) const {
        for (size_t i = 0; i < column_names.size(); ++i)
            if (column_names[i] == name) return static_cast<int>(i);
        return -1;
    }
    // 便捷：一行拿 int64_t 列数据裸指针（TIMESTAMP / INT 用）
    const int64_t* i64Data(size_t col) const { return columns[col].i64.data(); }
    // 便捷：一行拿 double 列数据裸指针（FLOAT 用）
    const double* f64Data(size_t col) const { return columns[col].f64.data(); }
};

// SQL 语句类型（Query() 返回的 QueryResult 中标识原始语句类型）。
enum class StatementType : uint8_t {
    SELECT = 0,
    CREATE_TABLE,
    INSERT,
    ALTER_ADD_COLUMN,
    ALTER_DROP_COLUMN,
    UPDATE,
};

// 统一查询结果（SELECT / DDL / DML 通用）。
// SELECT 时填充 column_*，rows 惰性物化或通过 Fetch() 逐块读取。
// DDL/DML 时使用 statement_type + rows_affected。
struct QueryResult {
    QueryResult() = default;
    ~QueryResult();                                       // 定义在 connection.cpp（PIMPL）
    QueryResult(QueryResult&&) = default;                 // unique_ptr<Impl> 移动即可
    QueryResult& operator=(QueryResult&&) = default;

    StatementType statement_type = StatementType::SELECT;  // 原始语句类型
    std::vector<std::string> column_names;                  // 选中列的名称
    std::vector<ColumnType> column_types;                   // 选中列的类型
    std::vector<TimePrecision> column_precisions;           // 选中列的精度（TIMESTAMP 用）
    mutable std::vector<std::vector<Value>> rows;           // 行数据，惰性物化后可用（mutable 支持 const RowCount）
    int64_t rows_affected = 0;                              // 影响行数（INSERT=1, UPDATE=N，DDL/SELECT=0）

    // 行数。若 impl_ 存在（Query 路径），首次调用触发惰性物化。
    size_t RowCount() const;
    size_t ColumnCount() const { return column_names.size(); }

    // 按索引或标签访问行：Row(0)、Row("first")、Row("last")。
    RowView Row(size_t i) const { return {column_names, rows[i]}; }
    RowView Row(std::string_view label) const {
        if (label == "first" && !rows.empty()) return {column_names, rows[0]};
        if (label == "last" && !rows.empty()) return {column_names, rows.back()};
        return {column_names, rows[0]};
    }

    // 列优先逐块读取。每次从磁盘读取最多 chunk_size 行，row_count==0 表示结束。
    // DDL/DML 语句返回空 Chunk。调用 Fetch() 后 rows 不再自动物化。
    Result<Chunk> Fetch() const;

    // 设置 Fetch() 每次返回的最大行数，默认 2048。
    void SetChunkSize(size_t n);

  private:
    friend class Connection;
    void MaterializeRows() const;  // 惰性：从磁盘全量物化到 rows（mutable 语义）

    struct Impl;
    mutable std::unique_ptr<Impl> impl_;  // 流式状态，惰性物化为逻辑 const
};

// 数据库连接。
// 每个 Connection 独立持有 Catalog 快照——创建后对表的增删不可见。
class Connection {
  public:
    explicit Connection(WaveDB& db);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 建表。schema 需包含至少一列。已存在的表返回 ALREADY_EXISTS。
    Status CreateTable(const TableSchema& schema);

    // 添加列。旧 Part 中该列自动返回默认值（0 / 0.0）。
    Status AddColumn(
        std::string_view table_name,
        std::string field_name,
        ColumnType type,
        TimePrecision prec = TimePrecision::MICRO);

    // 删除列。旧 Part 中该列的 .col 文件保留不删除（不重写历史），
    // 查询时不再返回该列。
    Status DropColumn(std::string_view table_name, std::string_view field_name);

    // 更新单列全量值。values 长度必须等于全表行数。
    Status UpdateColumn(std::string_view table_name, std::string_view col_name, const std::vector<Value>& values);

    // 按 ts 范围更新单列值。values 长度必须等于 [from_ts, to_ts] 内行数。
    Status UpdateColumn(
        std::string_view table_name,
        std::string_view col_name,
        Timestamp from_ts,
        Timestamp to_ts,
        const std::vector<Value>& values);

    // 单行插入。内部创建临时 Appender → AppendRow → Close。
    // 高频写入应使用 CreateAppender() 批量写入。
    Status Insert(std::string_view table_name, const std::vector<Value>& row);

    // 查询。
    //   columns: 默认 {"*"} 返回所有列
    //   from_ts: 时间下界（0 = 不限制）
    //   to_ts:   时间上界（0 = 不限制）
    //   limit:   返回行数上限（0 = 不限制），取时间范围尾部的行
    Result<QueryResult> Select(
        std::string_view table_name,
        const std::vector<std::string>& columns = {"*"},
        Timestamp from_ts = 0,
        Timestamp to_ts = 0,
        int64_t limit = 0);

    // 统一 SQL 查询接口（DuckDB 风格）。
    // 接受一条完整的 SQL 语句：CREATE TABLE / INSERT / SELECT / ALTER TABLE / UPDATE。
    // SELECT 返回带行数据的 QueryResult，DDL/DML 返回带 statement_type + rows_affected 的 QueryResult。
    // 解析失败返回 PARSE_ERROR。
    //
    // 执行流程：
    //   (1) 解析 SQL 文本 → 识别语句类型
    //   (2) 委托给对应的 CreateTable / Insert / Select / AddColumn / DropColumn / UpdateColumn
    //   (3) 将结果封装为统一的 QueryResult 返回
    Result<QueryResult> Query(std::string_view sql);

    // 创建批量写入器。高频写入时性能远优于逐行 Insert。
    // Appender 缓冲时不持锁，仅在 Flush/Close 时持锁写入磁盘。
    Result<Appender> CreateAppender(std::string_view table_name);

    // 列出数据库中所有表名。
    std::vector<std::string> ListTables() const;

    // 获取指定表的 schema，不存在返回 nullptr。
    const TableSchema* GetTableSchema(std::string_view name) const;

    // 返回关联的数据库实例。
    WaveDB& db();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;  // PIMPL 隐藏 Catalog 等内部依赖
};

}  // namespace wavedb
