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
// 默认配置
auto db = WaveDB::Open("/data/db");

// 只读模式
auto db = WaveDB::Open("/data/db", {.read_only = true});

// 自定义配置：n_ Part 最多 1000 行自动切分，Fetch 默认每次 1024 行
WaveDBConfig config;
config.max_rows_per_part = 1000;  // n_ Part 最大行数，0 = 默认 2048
config.chunk_size = 1024;          // Fetch() 默认 chunk 大小
auto db = WaveDB::Open("/data/db", config);
```

### 属性

```cpp
db->path();         // 数据目录路径
db->read_only();    // 是否只读
db->config();       // WaveDBConfig 拷贝
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

// 可选：设置合并策略
schema.mergeConfig().policy = MergePolicy::BY_HOUR;
schema.mergeConfig().merge_target_rows = 3500;  // m_ Part 目标行数

Status s = conn.CreateTable(schema);
// 成功    → OK
// 表已存在 → ALREADY_EXISTS
// 磁盘错误 → IO_ERROR
// 只读模式 → INVALID_ARGUMENT
```

**MergeConfig 说明：**

| 字段 | 类型 | 含义 |
|------|------|------|
| `policy` | `MergePolicy` | `NONE`(不合并) / `BY_HOUR` / `BY_DAY` / `BY_MONTH` |
| `merge_target_rows` | `int64_t` | m_ Part 目标行数，0 = 不限制（全取）。仅 `policy != NONE` 时有效 |

**SQL 方式（推荐）：**

```sql
-- 按小时合并，每个 m_ Part 最多 3500 行
CREATE TABLE kbars (
    ts TIMESTAMP(MICRO),
    open FLOAT,
    high FLOAT,
    low FLOAT,
    close FLOAT,
    vol INT
) MERGE BY HOUR MAX_ROWS 3500;

-- 仅按天合并（无行数限制）
CREATE TABLE ticks (
    ts TIMESTAMP(SECOND),
    price FLOAT,
    vol INT
) MERGE BY DAY;

-- 不合并
CREATE TABLE ticks (ts TIMESTAMP(SECOND), price FLOAT);
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
| `AppendRow` | 内存缓冲，零 I/O；达到 `max_rows_per_part` 上限自动写一个 Part |
| `Flush` | 将缓冲区剩余行写为一个 Part（可能小于上限） |
| `Close` | 等同于 `Flush` |

**Part 大小控制：** n_ Part 由 `WaveDBConfig.max_rows_per_part`（全局默认 2048）控制；m_ Part 由 `MERGE ... MAX_ROWS N`（`MergeConfig.merge_target_rows`）控制。`Flush()` 只管落盘时机，不管 Part 大小。

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

**WHERE 过滤：** `SELECT ... WHERE ts >= val [AND ts <= val]` 支持时间过滤。与 `Select()` 语义等价。WHERE 子句中时间戳字面量精度与列精度按 [精度自适应](#时间戳精度自适应) 规则自动适配。

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

## 时间戳精度自适应

SQL 查询中 `WHERE ts >= val [AND ts <= val]` 或 C++ `Select()` 的时间范围过滤，时间戳字面量精度与表列精度按照以下规则自动适配：

### `>=` 下界规则

| 输入精度 vs 列精度 | 处理 | 示例（列=DAY） |
|--------------------|------|----------------|
| 输入**细于**列 | **截断**到列精度周期起点 | `>= 20260101-10:50:00` → `>= 20260101 00:00:00` |
| 输入**等于**列 | 不变 | `>= 20260101` → `>= 20260101` |
| 输入**粗于**列 | 不变（已自然对齐） | `>= 20260101` 在 MICRO 列 → `>= 20260101 00:00:00.000000` |

### `<=` 上界规则

| 输入精度 | 处理 | 示例 |
|----------|------|------|
| DAY | 扩展到当天末 | `<= 20260101` → `<= 20260101 23:59:59.999999` |
| HOUR | 扩展到该小时末 | `<= 20260101-10` → `<= 20260101 10:59:59.999999` |
| MINUTE | 扩展到该分钟末 | `<= 20260101-10:50` → `<= 20260101 10:50:59.999999` |
| SECOND | 扩展到该秒末 | `<= 20260101-10:50:00` → `<= 20260101 10:50:00.999999` |
| MILLI | 扩展到该毫秒末 | `<= 20260101-10:50:00-123` → `<= 20260101 10:50:00.123999` |
| MICRO | 不变 | `<= 20260101-10:50:00-123456` 保持原值 |

**实现函数：**
- `TruncateToPrecision(ts, prec)` — 截断到指定精度
- `ExpandToPeriodEnd(ts, prec)` — 扩展到指定精度周期的末尾
- `TimestampLiteralPrecision(s)` — 从时间戳字面量推断精度

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
| Writer | `flock(LOCK_EX)` 每 `.col` 文件 | `ColumnFile::Open(exclusive=true)` → `fclose` 自动释放 |
| Reader | 无锁 | Reader 不调 `flock`，直接 `mmap`/`fread` 读已提交的 Part |
| UpdateColumn | `flock(LOCK_EX)` 每 `.col.tmp` 文件 | `Part::WriteColumn` 内部写 `.tmp` 期间持有 |

- 锁粒度是列文件级（`.col`），不同表/不同 Part/不同列的写入不互斥
- `flock` 是 advisory lock：Reader 不调 `flock`，写者持锁时读完全不受影响
- `UpdateColumn` 使用原子 rename（`.col.tmp` → `.col`），Reader 要么看到旧数据要么看到完整新数据
- 崩溃安全：`flock` 关联 fd 生命周期，进程崩溃自动释放

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
