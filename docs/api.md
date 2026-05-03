# WaveDB v0.1 API 手册

## 头文件

```cpp
#include "src/engine/wavedb.h"
#include "src/engine/connection.h"
```

## 基本用法

```cpp
using namespace wavedb;

// 打开数据库
auto db = WaveDB::Open("/path/to/data");
Connection conn(*db);

// 建表 → 写数据 → 查数据
conn.CreateTable(schema);
conn.Insert("table", {val1, val2, val3});
auto result = conn.Select("table");
```

---

## WaveDB

### 打开数据库

```cpp
// 读写模式（获取排他锁，同一时刻只允许一个写进程）
auto db = WaveDB::Open("/data/db");

// 只读模式（获取共享锁，允许多个读进程共存）
auto db = WaveDB::Open("/data/db", {.read_only = true});
```

### 属性

```cpp
db->path();       // 数据目录路径
db->read_only();  // 是否只读模式
```

### 多进程安全

写进程持有 `LOCK_EX` 排他锁，读进程持有 `LOCK_SH` 共享锁。同一时刻最多一个写进程或 N 个读进程。

| 模式 | 锁 | 并发 |
|------|-----|------|
| 读写 | `flock(LOCK_EX)` | 最多 1 个进程 |
| 只读 | `flock(LOCK_SH)` | 最多 N 个进程 |

---

## Connection

```cpp
Connection conn(db);
```

一个 `WaveDB` 实例可以创建多个 `Connection`，但 v0.1 中每个 Connection 独立加载 Catalog。

### CreateTable

```cpp
TableSchema schema("ticks");
schema.AddColumn("ts",    ColumnType::kTimestamp);
schema.AddColumn("price", ColumnType::kFloat);
schema.AddColumn("volume", ColumnType::kInt);

Status s = conn.CreateTable(schema);
// 表已存在 → kAlreadyExists
// 磁盘错误 → kIOError
// 只读模式 → kInvalidArgument
```

### Insert（单行便捷方法）

```cpp
// initializer_list 形式
Status s = conn.Insert("ticks", {ts_value, 100.5, 1000});

// 等价于 vector<Value>
std::vector<Value> row = {ts_value, 100.5, 1000LL};
Status s = conn.Insert("ticks", row);
```

每次 `Insert` 会打开列文件 → 写入 → 刷盘 → 关闭。适合单行写入，批量写入请用 `Appender`。

### CreateAppender（批量写入）

```cpp
// 创建 Appender
auto app = conn.CreateAppender("ticks");
if (!app.ok()) { /* 处理错误 */ }

// 逐行追加
app->AppendRow(ts_1, 100.5, 1000);
app->AppendRow(ts_2, 101.0, 1500);
app->AppendRow(ts_3, 100.8,  800);

// 可选：强制刷盘
app->Flush();

// 继续追加
app->AppendRow(ts_4, 101.5, 2000);

// 关闭（自动 Flush + 关闭文件）
app->Close();

size_t n = app->row_count();  // 已写入行数
```

**Appender 性能模型：**
- `AppendRow` → `fwrite`（写入 stdio 缓冲区，不落盘）
- `Flush()` → `fflush`（强制刷到 OS）
- `Close()` → `Flush()` + `fclose`（落盘 + 关文件）

### Select

```cpp
// SELECT * FROM ticks
auto r = conn.Select("ticks");

// SELECT price, volume FROM ticks
auto r = conn.Select("ticks", {"price", "volume"});

// SELECT * FROM ticks WHERE ts >= from_ts
auto r = conn.Select("ticks", {"*"}, from_ts);

// SELECT * FROM ticks WHERE ts >= from_ts AND ts <= to_ts
auto r = conn.Select("ticks", {"*"}, from_ts, to_ts);

// SELECT price FROM ticks WHERE ts >= from_ts
// （ts 不在结果列中，但在内部用于过滤）
auto r = conn.Select("ticks", {"price"}, from_ts, to_ts);
```

**参数说明：**

| 参数 | 类型 | 含义 |
|------|------|------|
| `table_name` | `string_view` | 表名 |
| `columns` | `vector<string>` | 列名列表，`{}` 或 `{"*"}` 表示全列 |
| `from_ts` | `Timestamp` (int64_t) | 时间下界（含），微秒。0 表示不限制 |
| `to_ts` | `Timestamp` (int64_t) | 时间上界（含），微秒。0 表示不限制 |

---

## QueryResult

```cpp
Result<QueryResult> r = conn.Select("ticks");

// 检查是否成功
if (!r.ok()) {
  std::cerr << r.status.message() << "\n";
  return;
}

// 元数据
r->column_names;      // ["ts", "price", "volume"]
r->column_types;      // [kTimestamp, kFloat, kInt]
r->column_precisions; // [SECOND, MICRO, MICRO]
r->RowCount();        // 行数
r->ColumnCount();     // 列数

// 遍历（行优先）
for (auto& row : r->rows) {
  if (r->column_types[i] == ColumnType::kTimestamp) {
    std::string formatted = FormatTimestamp(
        std::get<int64_t>(row[i]), r->column_precisions[i]);
    // "20260101-10:50:00"
  }
  // ...
}
```

---

## 时间戳

### 精度

CREATE TABLE 时可为每个 TIMESTAMP 列指定显示精度：

```cpp
schema.AddColumn("ts", ColumnType::kTimestamp, TimePrecision::SECOND);
schema.AddColumn("ts_us", ColumnType::kTimestamp, TimePrecision::MICRO);
```

| 精度 | FormatTimestamp 输出 | 示例 |
|------|----------------------|------|
| `DAY` | `YYYYMMDD` | `20260101` |
| `HOUR` | `YYYYMMDD-HH` | `20260101-10` |
| `MINUTE` | `YYYYMMDD-HH:MM` | `20260101-10:50` |
| `SECOND` | `YYYYMMDD-HH:MM:SS` | `20260101-10:50:00` |
| `MILLI` | `YYYYMMDD-HH:MM:SS-mmm` | `20260101-10:50:00-123` |
| `MICRO` | `YYYYMMDD-HH:MM:SS-mmmmmm` | `20260101-10:50:00-123456` |

### FormatTimestamp

```cpp
#include "src/common/types.h"

Timestamp ts = 1767264600000000LL;
std::cout << FormatTimestamp(ts, TimePrecision::SECOND);
// → "20260101-10:50:00"

std::cout << FormatTimestamp(ts, TimePrecision::MICRO);
// → "20260101-10:50:00-000000"
```

### ParseTimestamp

```cpp
// 输入可短于列精度——缺失部分自动补零
auto t1 = ParseTimestamp("20260101", TimePrecision::MICRO);
// → 20260101-00:00:00-000000 的微秒值

auto t2 = ParseTimestamp("20260101-10:50", TimePrecision::MICRO);
// → 20260101-10:50:00-000000 的微秒值

auto t3 = ParseTimestamp("20260101-10:50:00-123456", TimePrecision::MICRO);
// → 精确的微秒值
```

---

## 数据类型

| SQL 名 | C++ ColumnType | 存储类型 | 宽度 |
|--------|---------------|----------|------|
| TIMESTAMP | `kTimestamp` | `int64_t` | 8 bytes |
| FLOAT | `kFloat` | `double` | 8 bytes |
| INT | `kInt` | `int64_t` | 8 bytes |

`Value` 定义为 `std::variant<int64_t, double>`，INT 和 TIMESTAMP 共用 `int64_t`。

---

## 错误处理

```cpp
// Status — 错误码 + 消息
Status s = conn.Insert(...);
if (!s.ok()) {
  s.code();      // StatusCode 枚举
  s.message();   // 错误描述字符串
}

// Result<T> — 值或错误
Result<QueryResult> r = conn.Select(...);
if (r.ok()) {
  auto& data = *r;  // QueryResult&
  data.rows...
} else {
  r.status.message();
}
```

**Status 枚举：**

| 枚举 | 含义 |
|------|------|
| `kOk` | 成功 |
| `kNotFound` | 表/列不存在 |
| `kAlreadyExists` | 表已存在 |
| `kInvalidArgument` | 参数错误（列数不匹配、类型错误、只读模式写入） |
| `kParseError` | JSON 解析失败 |
| `kIOError` | 磁盘读写失败 |
| `kInternal` | 内部错误（锁冲突等） |
