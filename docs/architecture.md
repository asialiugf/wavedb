# WaveDB 架构设计

## 设计哲学

> Everything is a time-series signal. 一切皆时间序列信号。

### 优先级

1. 正确性
2. 文件格式稳定
3. 长期可维护性
4. 可调试性
5. 性能
6. 简洁性

### 核心理念

- **Append-only** — 从不修改历史数据，只追加
- **Columnar** — 每列一个 `.col` 文件，与 ClickHouse MergeTree 一致
- **Part 机制** — 每个 INSERT batch 生成一个不可变 Part，支持时间裁剪
- **显式 ownership** — unique_ptr, stack object, preallocated buffer
- **无异常** — Status / Result\<T\> 错误码
- **热路径零分配** — INSERT/scan 路径禁止 heap allocation, shared_ptr, virtual dispatch

---

## 分层架构

```
┌──────────────────────────────────────────┐
│  main.cpp         用户入口 / REPL         │
├──────────────────────────────────────────┤
│  engine/          面向用户的 C++ API      │
│  WaveDB           数据库实例 + flock 锁    │
│  Connection       连接（DDL/DML/DQL）      │
│  Appender         批量写入 → 生成 Part     │
│  QueryResult      查询结果集               │
├──────────────────────────────────────────┤
│  catalog/         元数据管理               │
│  Catalog          内存注册表 + 目录扫描     │
│  TableSchema      表结构定义 + JSON 序列化  │
├──────────────────────────────────────────┤
│  storage/         存储引擎                 │
│  ColumnFile       单列文件读写（底层原语）   │
│  Part             不可变分区（一组列文件）   │
│  PartManager      管理一张表的所有 Part     │
├──────────────┬───────────────────────────┤
│  parser/     │  common/                  │
│  （待实现）   │  基础类型 + 时间格式化      │
├──────────────┴───────────────────────────┤
```

## 数据流

```
INSERT 路径:
  Appender::AppendRow(ts, price, vol)
    → 内存缓冲（列优先，column-major）
    → 追踪 min/max ts
    → Appender::Close/Flush
    → Part::Create → 写 meta.json + 各列 .col 文件
    → 磁盘

SELECT 路径:
  Connection::Select("ticks", cols, from_ts, to_ts)
    → Catalog::GetTable → TableSchema
    → PartManager::Open → 扫描 parts/ 目录
    → GetPartsInRange(from_ts, to_ts) → 时间裁剪，跳过不重叠 Part
    → Part::ReadColumn → 列优先读取
    → 跨 Part 合并 + 行级时间过滤
    → QueryResult
```

## 存储格式

### 目录结构

```
data/<db_name>/
  .lock                     flock 锁文件
  <table_name>/
    schema.json             表结构定义
    parts/                  数据分区目录
      001/                  Part 目录（自增编号）
        meta.json           分区元数据
        ts.col              时间戳列
        price.col           价格列
        volume.col          成交量列
      002/
        meta.json
        ts.col
        price.col
        volume.col
```

### JSON 格式

**schema.json:**
```json
{
  "name": "ticks",
  "columns": [
    {"name": "ts",    "type": "TIMESTAMP", "precision": "SECOND"},
    {"name": "price", "type": "FLOAT"},
    {"name": "volume","type": "INT"}
  ]
}
```

**meta.json:**
```json
{
  "min_ts": 1767264600000000,
  "max_ts": 1767264900000000,
  "row_count": 5
}
```

### .col 文件格式

```
[value_0][value_1][value_2]...  (定长连续排列，无 header)
行数 = file_size / sizeof(value_type)
```

## Part 机制

### 核心概念

每个 INSERT batch 产生一个不可变 Part。Part 一经写入不再修改，这是 ClickHouse MergeTree 的核心设计。

### Part 生命周期

```
写入:  Appender::AppendRow → 内存缓冲
        Appender::Close → WritePart → Part::Create(dir)
         → mkdir + 写 meta.json + 逐列写 .col

读取:  PartManager::Open → 扫描 parts/ 目录
        → 加载所有 Part 的 meta.json
        
查询:  GetPartsInRange(from, to)
        → 跳过 max_ts < from 或 min_ts > to 的 Part（时间裁剪）
        
合并:  Part::ReadColumn(col) → 列优先读取
        → 跨 Part 拼接 → 行级过滤 → QueryResult
```

### 时间裁剪

每个 Part 记录 `min_ts` / `max_ts`，查询时只读取时间范围有交集的 Part：

```
Part 001: [10:00, 10:30]
Part 002: [10:30, 11:00]
Part 003: [11:00, 11:30]

WHERE ts BETWEEN 10:20 AND 10:40
  → 跳过 Part 003 (min_ts > 10:40)
  → 读取 Part 001 + 002
```

### 写入特性

- **不排序** — 假设用户按时序写入，排序留给后续 merge（v0.3）
- **Flush 语义** — 将已缓冲行写为一个 Part 并清空缓冲区，后续写入生成新 Part
- **Part 编号** — 自增整数 `001`, `002`... 构造时扫描目录确定下一个 ID

---

## 模块详解

### common/ — 零依赖基础层

| 文件 | 内容 |
|------|------|
| `types.h` | ColumnType, TimePrecision, Timestamp (int64_t微秒), Value (variant<int64_t,double>) |
| `status.h` | StatusCode 枚举, Status 类, Result\<T\> 模板 |
| `time_format.cpp` | FormatTimestamp, ParseTimestamp, 精度名映射 |

### catalog/ — 元数据管理

| 文件 | 内容 |
|------|------|
| `schema.h/.cpp` | ColumnDef(含precision), TableSchema, JSON 序列化/反序列化 |
| `catalog.h/.cpp` | Catalog 内存注册表, 目录扫描, 启动恢复, 建表持久化 |

- 启动时扫描 `data_dir/` 下所有 `schema.json`
- `CreateTable` → 建子目录 + 写 schema.json
- 列文件延迟创建（首次 INSERT 时创建 Part）

### storage/ — 存储引擎

| 文件 | 内容 |
|------|------|
| `column_file.h/.cpp` | ColumnFile: Open / Append / Flush / ReadAll / Close |
| `part.h/.cpp` | Part: 不可变分区，Create(写 meta.json + 列文件) / Open / ReadColumn |
| `part_manager.h/.cpp` | PartManager: 管理一张表的所有 Part，时间范围裁剪 |

- ColumnFile 使用 C99 `FILE*`（v0.3 换 mmap）
- Append → fwrite 到 stdio 缓冲区，Flush → fflush，Close → Flush + fclose
- ReadAll → rewind + fread 全量，行数 = fstat 文件大小 / 类型宽度（打开时快照）
- Part::Create → 逐列调用 ColumnFile 写入，最后写 meta.json
- PartManager::GetPartsInRange → 利用 min/max ts 跳过无关 Part

### engine/ — C++ API

| 文件 | 内容 |
|------|------|
| `wavedb.h/.cpp` | WaveDB: 数据库实例，持有 flock 锁 |
| `connection.h/.cpp` | Connection: DDL/DML/DQL, 持有 Catalog |
| `appender.h/.cpp` | Appender: 内存缓冲 → Close 生成 Part |

- `WaveDB::Open(path)` → LOCK_EX 排他锁
- `WaveDB::Open(path, {.read_only=true})` → LOCK_SH 共享锁
- `Connection` 内部持有 `Catalog`（每个连接独立加载）
- `Appender` 缓冲行数据（列优先），Close/Flush 时创建 Part

---

## 多进程一写多读

```
写进程:  WaveDB::Open(path)      → flock(LOCK_EX) → 排他
读进程:  WaveDB::Open(path,      → flock(LOCK_SH) → 共享
         {.read_only=true})

写进程持有排他锁期间，读进程阻塞等待。
写进程关闭/析构后，读进程自动获取共享锁。
多个读进程可同时持有共享锁。
```

---

## 时间戳处理

### 存储层

所有时间戳以 `int64_t` 微秒纪元存储，与精度无关。精度仅影响显示格式。

### 精度

CREATE TABLE 时每列 TIMESTAMP 指定精度：

```json
{"name": "ts", "type": "TIMESTAMP", "precision": "SECOND"}
{"name": "ts_us", "type": "TIMESTAMP", "precision": "MICRO"}
```

| 精度 | 格式 |
|------|------|
| DAY | `20260101` |
| HOUR | `20260101-10` |
| MINUTE | `20260101-10:50` |
| SECOND | `20260101-10:50:00` |
| MILLI | `20260101-10:50:00-123` |
| MICRO | `20260101-10:50:00-123456` |

### 解析

`ParseTimestamp(str, col_prec)` 接受任意精度的输入字符串，缺失部分自动补零。短输入 `"20260101"` 在 MICRO 列上自动补全为 `20260101-00:00:00-000000`。

### 输出

`FormatTimestamp(ts, col_prec)` 按列精度格式化。`QueryResult` 携带 `column_precisions`，输出层无需额外查 schema。

---

## 文件清单

```
src/
  common/
    types.h           — ColumnType, TimePrecision, Timestamp, Value
    status.h          — StatusCode, Status, Result<T>
    time_format.cpp   — FormatTimestamp, ParseTimestamp
  catalog/
    schema.h/.cpp     — ColumnDef(含precision), TableSchema, JSON
    catalog.h/.cpp    — Catalog, 目录扫描, 建表
  storage/
    column_file.h/.cpp — 列文件底层读写
    part.h/.cpp        — 不可变 Part
    part_manager.h/.cpp — Part 管理器 + 时间裁剪
  engine/
    wavedb.h/.cpp     — WaveDB + flock 锁
    connection.h/.cpp — Connection, QueryResult
    appender.h/.cpp   — Appender: 内存缓冲 → Part
  main.cpp            — 集成测试
docs/
  api.md              — API 使用手册
  architecture.md     — 架构设计（本文档）
```

## v0.3 计划

- SQL Parser（手写递归下降）
- Part 后台合并（Merge）
- WHERE 条件过滤（非 ts 列）
- mmap 列文件扫描
- WAL 写前日志
- 列压缩（Delta / RLE）
