# WaveDB v0.2 API 手册

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

### CreateTable

```cpp
TableSchema schema("ticks");
schema.AddColumn("ts",    ColumnType::kTimestamp, TimePrecision::SECOND);
schema.AddColumn("price", ColumnType::kFloat);
schema.AddColumn("volume", ColumnType::kInt);

Status s = conn.CreateTable(schema);
// 成功    → kOk
// 表已存在 → kAlreadyExists
// 磁盘错误 → kIOError
// 只读模式 → kInvalidArgument
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

### Select

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
| 表不存在 | `kNotFound` |
| 列不存在 | `kNotFound` |
| 空表 | `kOk`（返回 0 行） |
| 时间范围无数据 | `kOk`（返回 0 行） |

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
r->column_types;       // [kTimestamp, kFloat, kInt]
r->column_precisions;  // [SECOND, MICRO, MICRO]
r->RowCount();         // 行数（受 limit/过滤影响）
r->ColumnCount();      // 列数

// 遍历（行优先）
for (auto& row : r->rows) {
  for (size_t i = 0; i < r->ColumnCount(); ++i) {
    if (r->column_types[i] == ColumnType::kTimestamp) {
      std::cout << FormatTimestamp(std::get<int64_t>(row[i]),
                                   r->column_precisions[i]);
    } else if (r->column_types[i] == ColumnType::kFloat) {
      std::cout << std::get<double>(row[i]);
    } else {
      std::cout << std::get<int64_t>(row[i]);
    }
  }
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

---

## 数据类型

| ColumnType | 存储类型 | 宽度 | Value variant |
|------------|----------|------|---------------|
| `kTimestamp` | `int64_t` | 8 bytes | `int64_t` |
| `kFloat` | `double` | 8 bytes | `double` |
| `kInt` | `int64_t` | 8 bytes | `int64_t` |

`Value` = `std::variant<int64_t, double>`，INT 和 TIMESTAMP 共用 `int64_t` 分支。

---

## 多进程一写多读

| 角色 | 锁 | 锁持有时机 |
|------|-----|------------|
| Writer | `LOCK_EX` | 仅在 `Flush`/`Close` 写盘时短暂持有 |
| Reader | 无锁 | Reader 不持锁，直接读已提交的 Part |

- Writer 缓冲数据时不持锁，只有写盘瞬间获取 `LOCK_EX`
- Reader 直接读取磁盘上已提交的 Part，无需任何锁
- Writer 不阻塞 Reader，Reader 不阻塞 Writer
- 多个 Writer 之间通过 `LOCK_EX` 互斥（同一时刻仅一个写盘）

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
| `kOk` | 成功 |
| `kNotFound` | 表/列不存在 |
| `kAlreadyExists` | 表已存在 |
| `kInvalidArgument` | 参数错误（列数不匹配、类型错误、只读模式下写入） |
| `kParseError` | JSON 解析失败 |
| `kIOError` | 磁盘读写失败 |
| `kInternal` | 内部错误（锁冲突等） |
