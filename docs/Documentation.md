# WaveDB 使用手册

## 快速开始

```bash
# 构建
./scripts/build.sh

# 启动 CLI
./build/wavedb /tmp/mydb
```

```sql
-- 建表（按小时合并，每个 m_ Part 最多 3500 行）
CREATE TABLE kbars (
    ts TIMESTAMP(MICRO),
    open FLOAT,
    high FLOAT,
    low FLOAT,
    close FLOAT,
    vol INT
) MERGE BY HOUR MAX_ROWS 3500;

-- 写入
INSERT INTO kbars VALUES (20260511-10:30:00-000000, 100.0, 101.0, 99.0, 100.5, 5000);

-- 查询
SELECT * FROM kbars;
SELECT ts, close FROM kbars WHERE ts >= 20260511-10:00:00 AND ts <= 20260511-11:00:00 LIMIT 100;
```

---

## SQL 语法

### CREATE TABLE

```sql
CREATE TABLE 表名 (
    列名 类型[(精度)],
    ...
) [MERGE BY HOUR|DAY|WEEK|MONTH [MAX_ROWS N]];
```

**类型：** `TIMESTAMP[(精度)]`, `FLOAT`, `INT`

**精度：** `DAY`, `HOUR`, `MINUTE`, `SECOND`, `MILLI`, `MICRO`（默认 MICRO）

**MERGE：**
- 不写 `MERGE` → 不合并
- `MERGE BY HOUR MAX_ROWS 3500` → MAX_ROWS 优先，从最小 n_ 往上累加 3500 行合一个 m_
- `MERGE BY DAY`（无 MAX_ROWS）→ 同一天内的 n_ 全部合一个 m_
- `MERGE BY WEEK` → 同一周（周一~周日）内的 n_ 全部合一个 m_

### INSERT

```sql
INSERT INTO 表名 VALUES (值1, 值2, ...);
```

**值类型：** 整数(42, -1)、浮点(3.14)、时间戳字面量(20260101-09:30:00)

### SELECT

```sql
SELECT [*|列1,列2,...] FROM 表名
    [WHERE 列 op 时间戳 [AND 列 op 时间戳]]   -- op: >= <= > <
    [LIMIT N];
```

### ALTER TABLE

```sql
ALTER TABLE 表名 ADD COLUMN 列名 类型;   -- 或 ADD FIELD
ALTER TABLE 表名 DROP COLUMN 列名;       -- 或 DROP FIELD
```

### UPDATE

```sql
UPDATE 表名 SET 列名 = 值1, 值2, ...;
UPDATE 表名 SET 列名 = 值1, 值2, ... FROM 时间 TO 时间;
```

---

## 时间戳

### 字面量格式

| 精度 | 格式 | 示例 |
|------|------|------|
| DAY | `YYYYMMDD` | `20260511` |
| HOUR | `YYYYMMDD-HH` | `20260511-10` |
| MINUTE | `YYYYMMDD-HH:MM` | `20260511-10:30` |
| SECOND | `YYYYMMDD-HH:MM:SS` | `20260511-10:30:00` |
| MILLI | `YYYYMMDD-HH:MM:SS-mmm` | `20260511-10:30:00-123` |
| MICRO | `YYYYMMDD-HH:MM:SS-mmmmmm` | `20260511-10:30:00-123456` |

### WHERE 精度自适应

`>=` `<=` 用截断/扩展适配，`>` `<` 直接用 ±1µs：

| 操作符 | 规则 | 示例 |
|--------|------|------|
| `>=` | 输入细于列 → 截断 | `>= 20260101-10:50:00` 在 DAY 列 → `>= 20260101 00:00:00` |
| `>` | `from_ts = val + 1µs` | `> 20260101-10:50:00` → `> 20260101-10:50:00.000001` |
| `<=` | 扩展到周期末尾 | `<= 20260101` → `<= 20260101 23:59:59.999999` |
| `<` | `to_ts = val - 1µs` | `< 20260101-10:50:00` → `< 20260101-10:49:59.999999` |

```sql
SELECT * FROM t WHERE ts > 20260508 AND ts < 20260509;
```

---

## 配置

### WaveDBConfig（全局）

```cpp
WaveDBConfig config;
config.max_rows_per_part = 1000;   // n_ Part 最大行数，默认 2048
config.chunk_size = 1024;          // Fetch() chunk 大小，默认 2048
config.read_only = false;          // 只读模式

auto db = WaveDB::Open("/data/db", config);
```

### MergeConfig（表级，通过 SQL 设置）

```sql
CREATE TABLE t (...) MERGE BY HOUR MAX_ROWS 3500;
```

---

## C++ API

### 打开数据库

```cpp
#include "wavedb.h"
using namespace wavedb;

auto db = WaveDB::Open("/data/db");
Connection conn(*db);
```

### 建表

```cpp
// 方式 1：C++ API
TableSchema schema("ticks");
schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
schema.AddColumn("price", ColumnType::FLOAT);
schema.mergeConfig().policy = MergePolicy::BY_HOUR;
schema.mergeConfig().merge_target_rows = 3500;
conn.CreateTable(schema);

// 方式 2：SQL（推荐）
conn.Query("CREATE TABLE ticks (ts TIMESTAMP(SECOND), price FLOAT) MERGE BY HOUR MAX_ROWS 3500");
```

### 批量写入（高性能）

```cpp
auto app = conn.CreateAppender("ticks");
app->AppendRow(ts1, 100.5, 1000);   // 内存缓冲，零 I/O
app->AppendRow(ts2, 101.0, 1500);
app->Flush();                        // 写盘
app->AppendRow(ts3, 100.8, 800);
app->Close();                        // 写剩余 + 释放
```

### 查询

```cpp
// 全表
auto r = conn.Select("ticks");

// 投影 + 时间过滤 + limit
auto r = conn.Select("ticks", {"ts", "price"}, from_ts, to_ts, 100);

// 遍历
for (size_t i = 0; i < r->RowCount(); ++i) {
    auto row = r->Row(i);
    int64_t ts = row["ts"];
    double price = row["price"];
}

// 列优先逐块读取（大数据量）
while (true) {
    auto chunk = r->Fetch();
    if (chunk->row_count == 0) break;
    const int64_t* ts = chunk->i64Data(0);
    const double* price = chunk->f64Data(1);
}
```

### SQL 统一接口

```cpp
auto r = conn.Query("SELECT ts, price FROM ticks WHERE ts >= 20260511 LIMIT 100");
r->statement_type;  // StatementType::SELECT
```

### 修改表结构

```cpp
conn.AddColumn("ticks", "bid", ColumnType::FLOAT);       // 添加列
conn.DropColumn("ticks", "vol");                          // 删除列
conn.UpdateColumn("ticks", "price", new_prices);          // 全表更新
conn.UpdateColumn("ticks", "price", from_ts, to_ts, v);  // 范围更新
```

---

## 目录结构

```
data/<db_name>/
  <table_name>/
    schema.json                   表结构定义
    parts/
      .n_seq                       n_ 序号（格式 "20260511 15"）
      .m_seq                       m_ 序号（格式 "20260511 3"）
      n_20260511_000001/           普通 Part（INSERT 产生）
        meta.json                  时间范围 + 行数
        ts.col                     裸 int64_t 列文件
        price.col                  裸 double 列文件
      m_20260511_000001/           合并 Part（merge 产生）
        meta.json
        ts.col
        price.col
```

---

## 内部机制

### Part

- **n_ Part：** INSERT 写入产生，不可变。Appender 按 `max_rows_per_part` 自动拆分
- **m_ Part：** MergeScheduler 后台合并产生，不可变
- **无 merge_offset：** 合并部分消费后，剩余行直接重写 `.col` + 更新 `meta.json`

### Merge

- **MAX_ROWS：** 渐进式——找 in-progress m_，累加 n_ 直到填满 target，溢出开新 m_
- **纯 policy（无 MAX_ROWS）：** 渐进式——找 in-progress m_，同 boundary 的 n_ 追加，换 boundary 关闭
- **MergeScheduler：** 后台线程，每 5 秒唤醒扫描一次

### 一写多读

- Writer 写 `.col` 时加 `flock(LOCK_EX)`
- Reader 不加锁，直接读已提交的 Part
- 不同表/不同 Part/不同列的写入互不阻塞

### TS 去重

- `Appender::Flush` 时检查缓冲行 TS > 已有 Part 最大 TS
- TS 重复时清空 buffer 返回错误，防止死循环
