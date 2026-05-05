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
│  tools/           外部使用者              │
│  writer/reader    #include "wavedb.h"    │
│                   link libwavedb.a       │
├──────────────────────────────────────────┤
│  include/wavedb/  公开头文件             │
│  types.h status.h schema.h               │
│  database.h connection.h appender.h      │
├──────────────────────────────────────────┤
│  cli/             命令行交互工具          │
│  Shell            REPL 循环 + dot 命令    │
│  linenoise        行编辑 + 历史 + 补全    │
├──────────────────────────────────────────┤
│  parser/          SQL 解析器              │
│  Parser           递归下降解析            │
│  Tokenizer        关键字/字面量切分        │
│  ParseCallbacks   回调接口（无 AST）       │
├──────────────────────────────────────────┤
│  engine/          C++ API 实现           │
│  WaveDB           数据库实例（不持锁）     │
│  FileLock         操作级文件锁            │
│  Connection       连接（PIMPL）           │
│  Appender         批量写入 → 生成 Part    │
│  QueryResult      查询结果集              │
├──────────────────────────────────────────┤
│  catalog/         元数据管理              │
│  Catalog          内存注册表 + 目录扫描    │
│  TableSchema      表结构定义 + JSON       │
├──────────────────────────────────────────┤
│  storage/         存储引擎                │
│  ColumnFile       单列文件读写（底层原语） │
│  Part             不可变分区               │
│  PartManager      按 min_ts 排序 + 裁剪   │
├──────────────┬───────────────────────────┤
│  common/     │  （零依赖基础层）           │
│  基础类型 + 时间格式化                    │
└──────────────┴───────────────────────────┘
```

## 数据流

```
INSERT 路径:
  Appender::AppendRow(ts, price, vol)
    → 内存缓冲（列优先，column-major）
    → 追踪 min/max ts
    → 达到 max_rows_per_part 上限自动 WritePart（与 Flush 时机无关）
    → Flush/Close → 写剩余缓冲行为一个新的 Part
    → Part::Create → ColumnFile::Open(exclusive=true) 对每列 .col 加 flock
    → 写 meta.json + 各列 .col 文件
    → ColumnFile::Close 自动释放 flock

SELECT 路径:
  Connection::Select("ticks", cols, from_ts, to_ts, limit)
    → Catalog::GetTable → TableSchema
    → PartManager::Open → 扫描 parts/ 目录，按 min_ts 排序
    → GetPartsInRange(from_ts, to_ts) → 时间裁剪
    → Part::ReadColumn → 列优先读取 + 懒默认值（新增列旧Part自动补0）
    → 跨 Part 合并 + 行级时间过滤 + limit 截断
    → QueryResult

UPDATE 路径:
  Connection::UpdateColumn("ticks", "price", values)
    → PartManager::Open + GetPartsInRange
    → 校验 values 长度 = 总行数
    → 逐 Part 调用 Part::WriteColumn
    → ColumnFile::Open(tmp_path, exclusive=true) 对 .tmp 文件加 flock
    → 写 .col.tmp → rename(.col)（原子替换）
    → ColumnFile::Close 自动释放 flock

ALTER TABLE ADD FIELD 路径:
  Connection::AddColumn("ticks", "bid_price", FLOAT)
    → Catalog::AddColumn → 更新内存 schema + 重写 schema.json
    → 旧 Part 不重写——ReadColumn 检测 .col 文件为空时返回默认值（0/0.0）

ALTER TABLE DROP COLUMN 路径:
  Connection::DropColumn("ticks", "volume")
    → Catalog::DropColumn → 从 schema 移除列 + 重写 schema.json
    → 旧 Part 的 .col 文件保留不删除，查询时不再返回该列
```

## 存储格式

### 目录结构

```
data/<db_name>/
  <table_name>/
    schema.json             表结构定义
    parts/                  数据分区目录
      n_20260505_000001/    Part 目录（n=normal, m=merged, YYYYMMDD=数据日期, XXXXXX=当日编号）
        meta.json           分区元数据（min_ts/max_ts/row_count）
        ts.col              时间戳列（定长裸二进制）
        price.col           价格列
        volume.col          成交量列
      n_20260505_000002/
        meta.json
        ts.col
        price.col
        volume.col
      m_20260504_000001/    Merge 产物（5/4 ~ 8/1 数据合并于此，日期取最早 TS 日）
        meta.json
        ts.col
        price.col
        volume.col
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

每个 INSERT batch 产生一个不可变 Part。Part 一经写入不再修改。

### Part 生命周期

```
写入:  Appender::AppendRow → 内存缓冲（零 I/O）
        达到 max_rows_per_part → 自动 WritePart
        Flush/Close → 写剩余缓冲行为 Part
        Part::Create → ColumnFile::Open(exclusive=true) 逐列加 flock
        → mkdir + 写 meta.json + 逐列写 .col
        → ColumnFile::Close 自动释放 flock

读取:  PartManager::Open → 扫描 parts/ 目录
        → 加载所有 Part 的 meta.json
        → 按 min_ts 排序

查询:  GetPartsInRange(from, to)
        → 跳过 max_ts < from 或 min_ts > to 的 Part（时间裁剪）

合并:  Part::ReadColumn(col) → 列优先读取
        → 跨 Part 拼接 → 行级过滤 → QueryResult

更新:  Part::WriteColumn(col, values)
        → 写 .col.tmp → rename → .col（原子替换）
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

- **不排序** — 假设用户按时序写入，排序留给后续 merge
- **Part 大小** — 由 `WaveDBConfig.max_rows_per_part`（全局默认）或 `CREATE TABLE ... MAX_ROWS N`（表级）控制。`AppendRow` 达到上限自动切分新 Part，与 `Flush()` 调用频率无关
- **Flush 语义** — 将缓冲中剩余行写为一个 Part（可能小于上限），清空缓冲区。后续写入生成新 Part
- **Part 编号** — 格式 `n_YYYYMMDD_XXXXXX`（n-Part）或 `m_YYYYMMDD_XXXXXX`（m-Part）。日期来自数据 TS，同一天内序列号自增 000001→000002→...，跨天重置
- **列文件锁** — 每列 `.col` 文件写入时通过 `ColumnFile::Open(exclusive=true)` 加 `flock(LOCK_EX)`，`fclose` 自动释放。不同表/不同 Part/不同列写入不互斥

### Schema Evolution（懒默认值策略）

ALTER TABLE ADD FIELD 后新增列。旧 Part 中不存在该列的 `.col` 文件：

- `Part::ReadColumn` 检测 `.col` 文件为空且 `row_count > 0` 时
- 返回 `row_count` 个默认值（FLOAT → `0.0`，INT/TIMESTAMP → `0`）
- 旧 Part 不重写，数据文件完全不受影响

ALTER TABLE DROP COLUMN 后：

- 列从 `schema.json` 移除
- 旧 Part 中的 `.col` 文件保留不删除
- 查询时投影列表不包含已删除列，数据不可见

### 列更新（原子 WriteColumn）

`Part::WriteColumn` 实现单列原子更新：

- 先写 `.col.tmp` 临时文件
- 写完后 `rename(.col.tmp, .col)`（POSIX 原子操作）
- Reader 要么看到旧文件，要么看到完整新文件，不会看到半写状态

---

## 模块详解

### common/ — 零依赖基础层

| 文件 | 内容 |
|------|------|
| `types.h` | ColumnType（TIMESTAMP/FLOAT/INT）, TimePrecision（DAY-MICRO）, Timestamp（int64_t微秒）, Value（variant<int64_t,double>）, ColumnTypeSize |
| `status.h` | StatusCode 枚举（OK/NOT_FOUND/ALREADY_EXISTS/INVALID_ARGUMENT/PARSE_ERROR/IO_ERROR/INTERNAL）, Status 类, Result\<T\> 模板 |
| `time_format.cpp` | FormatTimestamp, ParseTimestamp, TimePrecisionName, TimePrecisionFromName |

### catalog/ — 元数据管理

| 文件 | 内容 |
|------|------|
| `schema.h/.cpp` | ColumnDef（含 precision）, TableSchema（AddColumn/DropColumn/ColumnIndex/FindColumn/RowByteSize）, JSON 序列化/反序列化 |
| `catalog.h/.cpp` | Catalog 内存注册表, 目录扫描（跳过隐藏目录和损坏 schema）, 启动恢复, CreateTable/AddColumn/DropColumn 持久化 |

- 启动时扫描 `data_dir/` 下所有 `schema.json`
- `CreateTable` → 建子目录 + 写 schema.json
- `AddColumn` → 更新内存 schema + 重写 schema.json（失败时回滚）
- `DropColumn` → 从 schema 移除列 + 重写 schema.json（失败时回滚）
- 损坏的 schema.json 静默跳过，不阻止数据库打开

### storage/ — 存储引擎

| 文件 | 内容 |
|------|------|
| `column_file.h/.cpp` | ColumnFile: Open（a+b模式） / Append / Flush / ReadAll / Close |
| `part.h/.cpp` | Part: 不可变分区，Create / Open / ReadColumn / WriteColumn（原子更新） |
| `part_manager.h/.cpp` | PartManager: 管理一张表的所有 Part，按 min_ts 排序，时间裁剪 |

- ColumnFile 使用 C99 `FILE*`（"a+b" 模式，append + read）
- Append → fwrite 到 stdio 缓冲区，Flush → fflush，Close → Flush + fclose
- ReadAll → rewind + fread 全量，行数 = stat.st_size / 类型宽度
- Part::Create → 逐列调用 ColumnFile 写入，最后写 meta.json（标记 Part 完成）
- Part::ReadColumn → 检测空列文件 → 返回懒默认值（schema evolution 支持）
- Part::WriteColumn → 写 .col.tmp → rename(.col)（原子替换）
- PartManager 加载后按 `min_ts` 排序，`GetPartsInRange` 利用 min/max ts 裁剪

### engine/ — C++ API

| 文件 | 内容 |
|------|------|
| `wavedb.h/.cpp` | WaveDB（路径 + 全局配置 WaveDBConfig），FileLock（操作级文件锁，RAII + flock，已下沉到 ColumnFile 内部使用） |
| `connection.h/.cpp` | Connection（PIMPL），QueryResult, Cell, RowView, Select, Insert, CreateTable, AddColumn, DropColumn, UpdateColumn, ListTables, GetTableSchema |
| `appender.h/.cpp` | Appender: 内存缓冲（列优先）→ Flush/Close 时创建 Part |

- `WaveDB` 不持有锁，只管理数据目录路径和全局配置（`WaveDBConfig`）
- `Connection` 使用 PIMPL 隐藏内部 Catalog 实现
- `Appender` 缓冲行数据（列优先），按 `max_rows_per_part` 自动切 Part；写盘时通过 `ColumnFile::Open(exclusive=true)` 对每列 `.col` 文件加 `flock`
- `Cell` 对 Value 的轻量包装，支持到 int64_t/double 的隐式转换
- `RowView` 按列名访问行值，返回 Cell

### parser/ — SQL 解析器

| 文件 | 内容 |
|------|------|
| `parser.h` | ParseCallbacks 回调接口（on_create_table/on_insert/on_select/on_add_column/on_drop_column/on_update_column），ParseSQL 入口 |
| `parser.cpp` | Tokenizer（关键字/字面量切分，大小写不敏感），Parser（递归下降解析，无 AST） |

- 手写递归下降解析器，零外部依赖
- 无 AST 中间表示——解析时直接通过 ParseCallbacks 回调执行
- 支持的 SQL：CREATE TABLE / INSERT / SELECT / ALTER TABLE ADD/DROP COLUMN/FIELD / UPDATE
- 类型：TIMESTAMP[(精度)] / FLOAT / INT
- 时间戳字面量：`20260101`, `20260101-09:30:00`, `20260101-09:30:00-123456`

### cli/ — 命令行交互工具

| 文件 | 内容 |
|------|------|
| `cli.h/.cpp` | Shell: REPL 循环, dot 命令分发（.help/.open/.close/.tables/.schema/.quit）, SQL 执行, 表格格式化输出 |

- 使用 linenoise 实现行编辑、历史记录（`.wavedb_history`）、Tab 补全
- Dot 命令：`.help`, `.open <path>`, `.close`, `.tables`, `.schema <table>`, `.quit`
- SQL 补全：关键字 + 当前数据库表名
- 输出格式：ASCII 表格（自动计算列宽）
- 两种模式：交互 REPL（`./wavedb [data_dir]`）、非交互执行（`./wavedb data_dir "SQL;"`）

### main.cpp — 入口

- `wavedb` 可执行文件入口
- 解析命令行参数（`--help` / `data_dir` / `SQL`）
- 创建 Shell 并启动 REPL 或执行单条 SQL

---

## 多进程一写多读

```
Writer（缓冲中）:  不持锁，内存缓冲，不影响 Reader
Writer（写列时）:  ColumnFile::Open(exclusive=true) → flock(LOCK_EX) 每 .col
                  → 写数据 → fclose 自动释放
Updater（写列时）: 同上，对 .col.tmp 文件加锁 → rename(.col) → 释放
Reader（始终）:    不调 flock，直接 fread 磁盘上已提交的 Part
```

- 锁粒度：列文件级（`.col`），不同表/不同 Part/不同列写入不互斥
- `flock` 是 advisory lock：Reader 不调 `flock`，写者持锁时读不受影响
- Part 不可变性保证已提交数据不变，Reader 数量无上限
- `UpdateColumn` 使用原子 rename，Reader 始终看到一致的数据版本
- 崩溃安全：`flock` 关联 fd 生命周期，进程崩溃自动释放

---

## 时间戳处理

### 存储层

所有时间戳以 `int64_t` 微秒纪元存储，与精度无关。精度仅影响显示格式。

### 精度

| 精度 | 格式 |
|------|------|
| DAY | `20260101` |
| HOUR | `20260101-10` |
| MINUTE | `20260101-10:50` |
| SECOND | `20260101-10:50:00` |
| MILLI | `20260101-10:50:00-123` |
| MICRO | `20260101-10:50:00-123456` |

### 解析

`ParseTimestamp(str, col_prec)` 接受任意精度的输入字符串，缺失部分自动补零。

### 输出

`FormatTimestamp(ts, col_prec)` 按列精度格式化。`QueryResult` 携带 `column_precisions`。

---

## 项目结构

```
wavedb/
  include/
    wavedb.h             顶层公开头文件
    wavedb/              模块头文件（types/status/schema/database/connection/appender）
  src/
    common/              基础类型 + 时间格式化实现
    catalog/             元数据管理实现（schema JSON + catalog 注册表）
    storage/             存储引擎实现（column_file + part + part_manager）
    engine/              API 实现（wavedb + connection + appender）
    parser/              SQL 解析器（tokenizer + 递归下降 parser）
    cli/                 命令行交互工具（Shell REPL）
    main.cpp             可执行文件入口
  tests/                 单元测试（Google Test，46+ 用例）
  tools/
    writer/              写进程示例（独立 CMakeLists.txt）
    reader/              读进程示例（独立 CMakeLists.txt）
  docs/
    api.md               API 使用手册
    architecture.md      架构设计（本文档）
    cli.md               命令行工具手册
  lib/                   make install 输出目录
  third_party/
    googletest/          测试框架
    linenoise.c          行编辑库
```

## v0.4 计划

- Part 后台合并（Merge）
- mmap 列文件扫描
- WAL 写前日志
- 列压缩（Delta / RLE）
- 多列 WHERE 条件过滤
