# WaveDB v0.3 API 手册

## 头文件

```cpp
#include "wavedb.h"
// 链接: -lwavedb
```

所有公开 API 在 `namespace wavedb` 下。

## 基本用法

```cpp
using namespace wavedb;

auto db  = WaveDB::Open("/path/to/data");
Connection conn(*db);

// 建表 → 写数据 → 查数据
conn.CreateTable(schema);
conn.Insert("table", {val1, val2, val3});
auto result = conn.Select("table");
```

---

## 构建与安装

```bash
cmake -B build
cmake --build build
cmake --install build --prefix .    # 安装到 ./lib 和 ./include
```

安装后使用者直接引用本地路径：

```cmake
# tools 的 CMakeLists.txt
target_include_directories(myapp PRIVATE ../../include)
target_link_directories(myapp PRIVATE ../../lib)
target_link_libraries(myapp wavedb pthread)
```

---

## WaveDB

### 打开数据库

```cpp
// 读写模式
auto db = WaveDB::Open("/data/db");

// 只读模式（写操作被拒绝）
auto db = WaveDB::Open("/data/db", {.read_only = true});
```

### 属性

```cpp
db->path();       // 数据目录路径
db->read_only();  // 是否只读
```

---

## Connection

```cpp
Connection conn(db);
```

### ListTables

```cpp
// 列出数据库中所有表名
std::vector<std::string> tables = conn.ListTables();
for (auto& t : tables) std::cout << t << "\n";
```

### GetTableSchema

```cpp
// 获取表 schema，不存在返回 nullptr
const TableSchema* schema = conn.GetTableSchema("ticks");
if (schema) std::cout << schema->ToJson();
```

### db

```cpp
// 返回关联的数据库实例
WaveDB& db = conn.db();
```

### CreateTable

```cpp
TableSchema schema("ticks");
schema.AddColumn("ts",    ColumnType::TIMESTAMP, TimePrecision::SECOND);
schema.AddColumn("price", ColumnType::FLOAT);
schema.AddColumn("volume", ColumnType::INT);

Status s = conn.CreateTable(schema);
// 成功    → OK
// 表已存在 → ALREADY_EXISTS
// 磁盘错误 → IO_ERROR
// 只读模式 → INVALID_ARGUMENT
```

### AddColumn（ALTER TABLE ADD FIELD）

```cpp
// 添加新列。旧 Part 中该列自动返回默认值（0 / 0.0）。
// 不重写历史数据。
Status s = conn.AddColumn("ticks", "bid_price", ColumnType::FLOAT);
// 成功    → OK
// 表不存在 → NOT_FOUND
// 列已存在 → ALREADY_EXISTS
```

### DropColumn（ALTER TABLE DROP COLUMN）

```cpp
// 删除列。旧 Part 中该列的 .col 文件保留不删除（不重写历史），
// 查询时该列不再出现在结果中。
Status s = conn.DropColumn("ticks", "volume");
// 成功    → OK
// 表不存在 → NOT_FOUND
// 列不存在 → NOT_FOUND
```

### UpdateColumn

```cpp
// ── 全表更新 ──────────────────────────────
// values 长度必须等于全表所有 Part 总行数
std::vector<Value> new_prices = {100.5, 101.0, 102.3};
Status s = conn.UpdateColumn("ticks", "price", new_prices);

// ── 按时间范围更新 ────────────────────────
// values 长度必须等于 [from_ts, to_ts] 内匹配的行数
int64_t from = ParseTimestamp("20260101-10:00", TimePrecision::MINUTE)->value;
int64_t to   = ParseTimestamp("20260101-11:00", TimePrecision::MINUTE)->value;
Status s = conn.UpdateColumn("ticks", "price", from, to, new_prices);

// 错误码：
//   成功 → OK
//   values 长度不匹配 → INVALID_ARGUMENT
//   表/列不存在 → NOT_FOUND
//   无 Part / 范围内无 Part → NOT_FOUND
//   只读模式 → INVALID_ARGUMENT
```

### Insert（单行便捷写入）

```cpp
Status s = conn.Insert("ticks", {ts_value, 100.5, 1000});
```

内部创建临时 Appender，写入一行后立即 Close 生成 Part。批量写入请用 `CreateAppender`。

### CreateAppender（批量写入）

```cpp
auto app = conn.CreateAppender("ticks");

// 逐行追加（缓冲在内存，不写盘）
app->AppendRow(ts_1, 100.5, 1000);
app->AppendRow(ts_2, 101.0, 1500);

// 写盘 + 清空缓冲，Appender 继续可用
app->Flush();

// 继续追加
app->AppendRow(ts_3, 100.8, 800);

// 关闭：写剩余数据 + 释放
app->Close();

size_t n = app->total_rows();  // 通过此 Appender 写入的总行数
```

**性能模型：**

| 操作 | 行为 |
|------|------|
| `AppendRow` | 内存缓冲，零 I/O |
| `Flush` | 获取 `LOCK_EX` → 写 Part 到磁盘 → 清空缓冲 → 释放锁 |
| `Close` | 等同于 `Flush` |

**与 Close 对比：** `Flush` 后 Appender 可继续追加；`Close` 后不可再用。长时间运行的写入场景应使用 `Flush` 避免反复重建 Appender。

### Query（统一 SQL 入口）

DuckDB 风格统一接口，一条 SQL 处理所有操作：

```cpp
// ── DDL ──────────────────────────────────
auto r = conn.Query("CREATE TABLE ticks (ts TIMESTAMP(SECOND), price FLOAT, vol INT)");
r->statement_type;  // StatementType::CREATE_TABLE
r->rows_affected;   // 0

// ── DML ──────────────────────────────────
auto r = conn.Query("INSERT INTO ticks VALUES (20260101-09:30:00, 100.5, 1000)");
r->statement_type;  // StatementType::INSERT
r->rows_affected;   // 1

// ── SELECT（惰性，不立即读数据）────────────
auto r = conn.Query("SELECT ts, price FROM ticks");

// 方式 1：逐块列优先访问（Fetch）
while (true) {
    auto chunk = r->Fetch();
    if (chunk->row_count == 0) break;
    const int64_t* ts    = chunk->i64Data(0);
    const double*  price = chunk->f64Data(1);
    for (size_t i = 0; i < chunk->row_count; ++i)
        std::cout << ts[i] << " " << price[i] << "\n";
}

// 方式 2：行优先全量访问（首次 RowCount() 触发惰性物化）
size_t n = r->RowCount();
for (size_t i = 0; i < n; ++i) {
    auto row = r->Row(i);
    int64_t ts = row["ts"];
    double price = row["price"];
}

// ── ALTER TABLE ──────────────────────────
conn.Query("ALTER TABLE ticks ADD COLUMN bid FLOAT");
conn.Query("ALTER TABLE ticks DROP COLUMN vol");

// ── UPDATE ───────────────────────────────
conn.Query("UPDATE ticks SET price = 99.5, 100.0, 101.0");
// 范围更新：
conn.Query("UPDATE ticks SET price = 999 FROM 20260101-09:31:00 TO 20260101-09:32:00");
```

**Query() 执行流程：**
- SELECT：只加载元信息（Part 列表），不读数据。`Fetch()` 逐块从磁盘读取。`RowCount()`/`Row()` 首次访问时全量物化。
- DDL/DML：直接执行，返回 `statement_type` + `rows_affected`

**注意：** v1 的 SELECT 不支持 WHERE 时间过滤和 LIMIT。如需过滤请使用 `Select()`。

### Select（参数化查询）

```cpp
// ── 基本查询 ──────────────────────────────

// SELECT * FROM ticks
auto r = conn.Select("ticks");

// SELECT price, volume FROM ticks
auto r = conn.Select("ticks", {"price", "volume"});

// ── 时间范围过滤 ──────────────────────────

// WHERE ts >= from_ts
auto r = conn.Select("ticks", {"*"}, from_ts);

// WHERE ts >= from_ts AND ts <= to_ts
auto r = conn.Select("ticks", {"*"}, from_ts, to_ts);

// 投影 + 时间过滤（ts 可以不在结果列中）
auto r = conn.Select("ticks", {"price"}, from_ts, to_ts);

// ── 限制行数（取尾部 N 行）────────────────

// 返回最后 3 行
auto r = conn.Select("ticks", {"*"}, 0, 0, 3);

// ── 全部参数组合 ──────────────────────────

// 投影 + 时间范围 + limit
auto r = conn.Select("ticks", {"ts", "price"},
                     from_ts, to_ts, 100);
```

**完整签名：**

```cpp
Result<QueryResult> Select(
    std::string_view table_name,
    const std::vector<std::string>& columns = {"*"},
    Timestamp from_ts = 0,
    Timestamp to_ts = 0,
    int64_t limit = 0);
```

**参数说明：**

| 参数 | 类型 | 默认值 | 含义 |
|------|------|--------|------|
| `table_name` | `string_view` | — | 表名（必填） |
| `columns` | `vector<string>` | `{"*"}` | 列名列表，`{}` 或 `{"*"}` 表示全列 |
| `from_ts` | `Timestamp` | `0` | 时间下界（含），微秒。0 表示不限制 |
| `to_ts` | `Timestamp` | `0` | 时间上界（含），微秒。0 表示不限制 |
| `limit` | `int64_t` | `0` | 返回尾部 N 行。0 表示全部。>0 时取最后 N 行 |

**错误码：**

| 场景 | StatusCode |
|------|------------|
| 表不存在 | `NOT_FOUND` |
| 列不存在 | `NOT_FOUND` |
| 空表 | `OK`（返回 0 行） |
| 时间范围无数据 | `OK`（返回 0 行） |

**时间裁剪：** `from_ts`/`to_ts` 非零时，PartManager 会利用每个 Part 的 `min_ts`/`max_ts` 跳过不相关的 Part，避免全表扫描。

---

## QueryResult

```cpp
Result<QueryResult> r = conn.Select("ticks");

if (!r.ok()) {
  std::cerr << r.status.message() << "\n";
  return;
}

// 元数据
r->column_names;       // ["ts", "price", "volume"]
r->column_types;       // [TIMESTAMP, FLOAT, INT]
r->column_precisions;  // [SECOND, MICRO, MICRO]
r->RowCount();         // 行数（受 limit/过滤影响）
r->ColumnCount();      // 列数

// ── 遍历（行优先，按索引访问） ──────────────
for (auto& row : r->rows) {
  for (size_t i = 0; i < r->ColumnCount(); ++i) {
    if (r->column_types[i] == ColumnType::TIMESTAMP) {
      std::cout << FormatTimestamp(std::get<int64_t>(row[i]),
                                   r->column_precisions[i]);
    } else if (r->column_types[i] == ColumnType::FLOAT) {
      std::cout << std::get<double>(row[i]);
    } else {
      std::cout << std::get<int64_t>(row[i]);
    }
  }
}

// ── RowView 遍历（按列名访问） ──────────────
auto first = r->Row("first");   // 第一行
auto last  = r->Row("last");    // 最后一行
auto idx2  = r->Row(2);         // 第 3 行

// 按列名取值（Cell 自动隐式转换到 int64_t/double）
int64_t ts = first["ts"];       // 隐式转换
double price = idx2["price"];

// 按索引取值
int64_t ts2 = first.At(0);
```

### Cell（单元格隐式转换）

`Cell` 是对 `Value`（`std::variant<int64_t, double>`）的轻量包装，支持到 `int64_t` 和 `double` 的隐式转换，省去 `std::get`。

```cpp
struct Cell {
    const Value& val;
    operator int64_t() const { return std::get<int64_t>(val); }
    operator double() const { return std::get<double>(val); }
};
```

### RowView（行视图）

`RowView` 按列名访问值，返回 `Cell`。

```cpp
struct RowView {
    const std::vector<std::string>& col_names;
    const std::vector<Value>& row_data;

    Cell operator[](std::string_view col_name) const;  // 按列名查找，O(n)
    Cell At(size_t i) const;                            // 按索引直接访问，O(1)
};
```

### Fetch（列优先逐块读取）

`Query("SELECT...")` 返回惰性结果，`Fetch()` 每次从磁盘读取最多 `chunk_size`（默认 2048）行，零全量内存分配：

```cpp
auto r = conn.Query("SELECT ts, price, volume FROM ticks");

// 可选：设置块大小
r->SetChunkSize(1024);

while (true) {
    auto chunk = r->Fetch();
    if (chunk->row_count == 0) break;  // 无更多数据

    // 方式 1：一行拿裸指针（根据列类型选 i64Data 或 f64Data）
    const int64_t* ts    = chunk->i64Data(0);
    const double*  price = chunk->f64Data(1);
    const int64_t* vol   = chunk->i64Data(2);

    // 方式 2：直接访问 ColumnChunk（精确控制）
    const int64_t* ts2 = chunk->columns[0].i64.data();

    // 方式 3：按列名查找索引
    int pi = chunk->ColumnIndex("price");
    const double* price2 = chunk->f64Data(pi);

    for (size_t i = 0; i < chunk->row_count; ++i)
        std::cout << ts[i] << " " << price[i] << " " << vol[i] << "\n";
}
```

### ColumnChunk（列数据块）

存储一列在 `[row_offset, row_offset + row_count)` 范围内的所有行数据：

```cpp
struct ColumnChunk {
    std::vector<int64_t> i64;  // TIMESTAMP / INT 数据
    std::vector<double> f64;   // FLOAT 数据
    ColumnType type;           // 列类型
    size_t size() const;       // i64.size() 或 f64.size()
};
```

### Chunk（数据块）

每次 `Fetch()` 返回一个 `Chunk`，包含所有选中列的列优先数据：

```cpp
struct Chunk {
    std::vector<std::string> column_names;          // 列名
    std::vector<ColumnType> column_types;           // 列类型
    std::vector<TimePrecision> column_precisions;   // 时间精度
    std::vector<ColumnChunk> columns;               // 每列一个 ColumnChunk
    size_t row_count = 0;                           // 本块行数

    size_t ColumnCount() const;                     // 列数
    const int64_t* i64Data(size_t col) const;       // 便捷：拿 int64 裸指针
    const double*  f64Data(size_t col) const;       // 便捷：拿 double 裸指针
    int ColumnIndex(std::string_view name) const;   // 按列名查找索引
};
```

**注意：** 调用 `Fetch()` 后 `rows` 不再自动物化。如需行优先访问，先调用 `RowCount()` 触发全量物化。

---

## 时间戳

### 精度

CREATE TABLE 时可为每个 TIMESTAMP 列指定显示精度：

```cpp
schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
schema.AddColumn("ts_us", ColumnType::TIMESTAMP, TimePrecision::MICRO);
```

| 精度 | 输出格式 | 示例 |
|------|----------|------|
| `DAY` | `YYYYMMDD` | `20260101` |
| `HOUR` | `YYYYMMDD-HH` | `20260101-10` |
| `MINUTE` | `YYYYMMDD-HH:MM` | `20260101-10:50` |
| `SECOND` | `YYYYMMDD-HH:MM:SS` | `20260101-10:50:00` |
| `MILLI` | `YYYYMMDD-HH:MM:SS-mmm` | `20260101-10:50:00-123` |
| `MICRO` | `YYYYMMDD-HH:MM:SS-mmmmmm` | `20260101-10:50:00-123456` |

### FormatTimestamp

```cpp
Timestamp ts = 1767264600000000LL;

FormatTimestamp(ts, TimePrecision::SECOND);  // "20260101-10:50:00"
FormatTimestamp(ts, TimePrecision::MICRO);   // "20260101-10:50:00-000000"
```

### ParseTimestamp

输入可短于列精度——缺失部分自动补零：

```cpp
auto t1 = ParseTimestamp("20260101", TimePrecision::MICRO);
// → 20260101-00:00:00-000000 的微秒值

auto t2 = ParseTimestamp("20260101-10:50", TimePrecision::MICRO);
// → 20260101-10:50:00-000000 的微秒值

auto t3 = ParseTimestamp("20260101-10:50:00-123456", TimePrecision::MICRO);
// → 精确的微秒值
```

### 精度名转换

```cpp
TimePrecisionName(TimePrecision::SECOND);    // "SECOND"
TimePrecisionFromName("MINUTE");             // TimePrecision::MINUTE
```

---

## 数据类型

| ColumnType | 存储类型 | 宽度 | Value variant |
|------------|----------|------|---------------|
| `TIMESTAMP` | `int64_t` | 8 bytes | `int64_t` |
| `FLOAT` | `double` | 8 bytes | `double` |
| `INT` | `int64_t` | 8 bytes | `int64_t` |

`Value` = `std::variant<int64_t, double>`，INT 和 TIMESTAMP 共用 `int64_t` 分支。

### ColumnTypeSize

```cpp
ColumnTypeSize(ColumnType::TIMESTAMP);  // 8
ColumnTypeSize(ColumnType::FLOAT);      // 8
ColumnTypeSize(ColumnType::INT);        // 8
```

---

## 多进程一写多读

| 角色 | 锁 | 锁持有时机 |
|------|-----|------------|
| Writer | `LOCK_EX` | 仅在 `Flush`/`Close` 写盘时短暂持有 |
| Reader | 无锁 | Reader 不持锁，直接读已提交的 Part |
| UpdateColumn | `LOCK_EX` | 在逐 Part 写 .col 文件期间持有 |

- Writer 缓冲数据时不持锁，只有写盘瞬间获取 `LOCK_EX`
- Reader 直接读取磁盘上已提交的 Part，无需任何锁
- Writer 不阻塞 Reader，Reader 不阻塞 Writer
- 多个 Writer 之间通过 `LOCK_EX` 互斥（同一时刻仅一个写盘）
- `UpdateColumn` 使用原子 rename（`.col.tmp` → `.col`），Reader 要么看到旧数据要么看到完整新数据

---

## 错误处理

```cpp
// Status — 错误码 + 消息
Status s = conn.Insert(...);
if (!s.ok()) {
  s.code();      // StatusCode 枚举
  s.message();   // 错误描述
}

// Result<T> — 值或错误
Result<QueryResult> r = conn.Select(...);
if (r.ok()) {
  auto& data = *r;
} else {
  r.status.message();
}
```

| StatusCode | 含义 |
|------------|------|
| `OK` | 成功 |
| `NOT_FOUND` | 表/列/Part 不存在 |
| `ALREADY_EXISTS` | 表/列已存在 |
| `INVALID_ARGUMENT` | 参数错误（列数不匹配、类型错误、只读模式下写入） |
| `PARSE_ERROR` | SQL 或 JSON 解析失败 |
| `IO_ERROR` | 磁盘读写失败 |
| `INTERNAL` | 内部错误（锁冲突等） |
