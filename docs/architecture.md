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
- **Columnar** — 每列一个 `.col` 文件
- **显式 ownership** — unique_ptr, stack object, preallocated buffer
- **无异常** — Status / Result\<T\> 错误码
- **热路径零分配** — INSERT/scan 路径禁止 heap allocation, shared_ptr, virtual dispatch
- **简单 parser** — 手写递归下降，不引入 AST framework

---

## 分层架构

```
┌──────────────────────────────────────────┐
│  main.cpp         用户入口 / REPL         │
├──────────────────────────────────────────┤
│  engine/          面向用户的 C++ API      │
│  WaveDB           数据库实例               │
│  Connection       连接（DDL/DML/DQL）      │
│  Appender         批量写入器               │
│  QueryResult      查询结果集               │
├──────────────────────────────────────────┤
│  catalog/         元数据管理               │
│  Catalog          内存注册表 + 目录扫描     │
│  TableSchema      表结构定义 + JSON 序列化  │
├──────────────┬───────────────────────────┤
│  storage/    │  parser/（待实现）         │
│  ColumnFile  │  SQL → AST → Connection   │
│  列文件读写   │                            │
├──────────────┴───────────────────────────┤
│  common/         基础类型                 │
│  ColumnType, Timestamp, Value            │
│  StatusCode, Status, Result<T>           │
└──────────────────────────────────────────┘
```

## 数据流

```
INSERT 路径:
  Appender::AppendRow(ts, price, vol)
    → Value → std::variant 拆包
    → ColumnFile::Append → fwrite → stdio 缓冲
    → Appender::Flush/Close → fflush/fclose → 磁盘

SELECT 路径:
  Connection::Select("ticks", cols, from_ts, to_ts)
    → Catalog::GetTable → TableSchema
    → ColumnFile::Open → 读取 file_size 快照行数
    → ColumnFile::ReadAll → fread 全量
    → 时间过滤（列优先 → 行优先转换时裁剪）
    → QueryResult
```

## 存储格式

```
data/
  <db_name>/
    .lock              flock 锁文件
    <table_name>/
      schema.json      表结构
      <col_1>.col      裸二进制列文件
      <col_2>.col
      ...

schema.json:
{
  "name": "ticks",
  "columns": [
    {"name": "ts",    "type": "TIMESTAMP"},
    {"name": "price", "type": "FLOAT"},
    {"name": "volume","type": "INT"}
  ]
}

.col 文件格式:
  [value_0][value_1][value_2]...  (定长连续排列，无 header)
  行数 = file_size / sizeof(value_type)
```

## 模块详解

### common/ — 零依赖基础层

| 文件 | 内容 |
|------|------|
| `types.h` | ColumnType 枚举, Timestamp (int64_t微秒), Value (variant<int64_t,double>) |
| `status.h` | StatusCode 枚举, Status 类, Result\<T\> 模板 |

### catalog/ — 元数据管理

| 文件 | 内容 |
|------|------|
| `schema.h/.cpp` | ColumnDef, TableSchema, JSON 序列化/反序列化 |
| `catalog.h/.cpp` | Catalog 内存注册表, 目录扫描, 启动恢复, 建表持久化 |

- 启动时扫描 `data_dir/` 下所有 `schema.json`
- `CreateTable` → 建子目录 + 写 schema.json
- 列文件延迟创建（首次 INSERT 时创建）

### storage/ — 列文件读写

| 文件 | 内容 |
|------|------|
| `column_file.h/.cpp` | ColumnFile: Open / Append / Flush / ReadAll / Close |

- 使用 C99 `FILE*`（v0.2 换 mmap）
- Append → fwrite 到 stdio 缓冲区
- Flush → fflush 刷到 OS
- Close → Flush + fclose
- ReadAll → rewind + fread 全量
- 行数 = fstat 文件大小 / 类型宽度（打开时快照，避免读到未落盘数据）

### engine/ — C++ API

| 文件 | 内容 |
|------|------|
| `wavedb.h/.cpp` | WaveDB: 数据库实例，持有 flock 锁 |
| `connection.h/.cpp` | Connection: DDL/DML/DQL, 持有 Catalog |
| `appender.h/.cpp` | Appender: 批量写入，保持 ColumnFile 打开 |

- `WaveDB::Open(path)` → LOCK_EX 排他锁
- `WaveDB::Open(path, {.read_only=true})` → LOCK_SH 共享锁
- `Connection` 内部持有 `Catalog`（每个连接独立加载）
- `Appender` 持有 `vector<ColumnFile>`，持续追加直到 Close

## 多进程一写多读

```
写进程:  WaveDB::Open(path)      → flock(LOCK_EX) → 排他
读进程:  WaveDB::Open(path,      → flock(LOCK_SH) → 共享
         {.read_only=true})

写进程持有排他锁期间，读进程阻塞等待。
写进程关闭/析构后，读进程自动获取共享锁。
多个读进程可同时持有共享锁。
```

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

## v0.1 文件清单

```
src/
  common/
    types.h           ( 49) — ColumnType, TimePrecision, Timestamp, Value
    status.h          ( 62) — StatusCode, Status, Result<T>
    time_format.cpp   (143) — FormatTimestamp, ParseTimestamp
  catalog/
    schema.h          ( 69) — ColumnDef(含precision), TableSchema
    schema.cpp        (197) — JSON 序列化/反序列化
    catalog.h         ( 38) — Catalog 声明
    catalog.cpp       (134) — 目录扫描、建表、schema.json 读写
  storage/
    column_file.h     ( 68) — ColumnFile 声明
    column_file.cpp   (109) — 列文件 append/Flush/scan
  engine/
    wavedb.h          ( 54) — WaveDB 声明 + Config
    wavedb.cpp        ( 49) — flock 锁实现
    connection.h      ( 50) — Connection, QueryResult(含precision)
    connection.cpp    (161) — DDL/DML/DQL 实现
    appender.h        ( 42) — Appender 声明 + variadic AppendRow
    appender.cpp      ( 62) — 批量写入实现
  main.cpp            (163) — 集成测试（含时间精度测试）
docs/
  api.md             — API 使用手册
  architecture.md    — 架构设计（本文档）
```

## v0.2 计划

- SQL Parser（手写递归下降）
- WHERE 条件过滤
- mmap 列文件扫描
- WAL 写前日志
- 列压缩（Delta / RLE）
