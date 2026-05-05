# Release Notes

## 2026-05-06 — WHERE 精度自适应 + Bug 修复

### 新功能：WHERE 时间戳精度自适应

`SELECT ... WHERE ts >= val [AND ts <= val]` 中，时间戳字面量精度与建表列精度按规则自适应：

| 运算符 | 条件 | 规则 | 示例 |
|--------|------|------|------|
| `>=` | 输入精度 **细于** 列精度 | **截断**到列精度周期起点 | `WHERE ts >= 20260101-10:50:00` 在 DAY 列 → `2026-01-01 00:00:00` |
| `>=` | 输入精度 **粗于/等于** 列精度 | 不变（自动补零即起点） | `WHERE ts >= 20260101` 在 MICRO 列 → `2026-01-01 00:00:00.000000` |
| `<=` | 任意 | **扩展**到输入精度周期末尾 | `WHERE ts <= 20260101` 在 MICRO 列 → `2026-01-01 23:59:59.999999` |

新增函数：`TruncateToPrecision`、`ExpandToPeriodEnd`、`TimestampLiteralPrecision`。

### 新功能：WHERE 语法增强

- 支持 `WHERE col <= val`（仅上界，之前只支持 `>=`）
- 支持 `WHERE col <= val AND col >= val`（AND 两侧顺序无关）

### Bug 修复

| Bug | 文件 | 修复 |
|-----|------|------|
| `yyjson_get_int` 32 位截断 micro-timestamps | `part.cpp` | 全部改用 `yyjson_get_sint`（int64_t） |
| `PartManager::Open` 严格命名导致非标准 Part 跳过 | `part_manager.cpp` | 改为检查 `meta.json` 存在性 |
| `Fetch()` 用 `row_count()` 跳过合并后 Part 的残留行 | `connection.cpp` | 改用 `effective_row_count()` |
| `on_select` 回调忽略 `from_ts/to_ts/limit` | `connection.cpp` | 有过滤时走带行级过滤的路径 |
| 重复头文件 `schema.h`、`appender.h` | `catalog/`、`engine/` | 删除，统一用 `include/wavedb/` |
| `Appender` 死代码 `next_part_id_` | `appender.h` | 移除 |

### 测试新增

- `tests/test_projection.cpp` — 18 个独立测试（列投影 + WHERE 精度自适应）
- `tests/test_appender.cpp` — 9 个独立测试（Appender 写入、缓冲区、校验）
- `test_harness.h` 新增 `#include "wavedb/status.h"`（自包含）

---


### 变更概要

- Part 目录命名从 `n000001` 改为 `n_YYYYMMDD_XXXXXX`，日期来自数据 TS（非系统时钟）
- 同一天内序列号自增（000001 → 000002 → ...），跨天重置
- m-Part 的日期取自 merge boundary 对应日期

### 改动汇总

| 文件 | 变化 |
|------|------|
| `part_manager.cpp` | 新增 `TsToDate`；`ParsePartId` → `ParsePartSeq`（解析 17 位格式）；`ScanNextPartId` → `ScanNextPartSeq(prefix, date)`；`MakePartDir` 增加 date 参数；`CreatePart`/`MergeParts` 使用 TS 日期 |
| `appender.cpp` | 新增 `TsToDate`；`ScanNextPartId` → `ScanNextPartSeq`；`NextPartDir` 从 `batch_min_ts_` 取日期 |
| `architecture.md` | 更新 Part 目录格式 |

---

## 2026-05-05 — Part 大小可配置 + Flush 与 Part 边界解耦

### 变更概要

- `WaveDBConfig` 新增 `max_rows_per_part` 和 `chunk_size` 两个全局配置项
- `Appender` 在 `AppendRow` 内部按 `max_rows_per_part` 自动切分 Part，不再依赖 `Flush()` 频率
- `Flush()` 职责回归纯粹：将缓冲数据落盘（可能生成小于上限的 Part）
- 表级 `MERGE BY HOUR MAX_ROWS N` 优先于全局 config

### 改动点

1. `WaveDBConfig` 新增 `max_rows_per_part`（0=不限制）和 `chunk_size`（默认 2048）
2. `Appender` 构造函数接收 `max_rows_per_part`，`AppendRow` 达到上限自动 `WritePart`
3. `Connection::CreateAppender` / `Insert` 读取表级 MergeConfig → fallback 全局 config
4. `QueryResult::Impl` 初始化 `chunk_size` 从 config
5. `writer.cpp` Open 时设置 config

### 改动汇总

| 文件 | 变化 |
|------|------|
| `database.h`, `wavedb.h` | `WaveDBConfig` 新增 `max_rows_per_part`、`chunk_size`；`WaveDB` 新增 `config_` + `config()` |
| `appender.h/cpp` | 构造接收 `max_rows_per_part`；`AppendRow` 达到上限自动 `WritePart` |
| `connection.cpp` | `CreateAppender`/`Insert` 表级 MergeConfig 优先；`QueryResult` 从 config 取 chunk_size |
| `writer.cpp` | `Open` 时传入 `WaveDBConfig{max_rows_per_part=1000, chunk_size=1024}` |

---

## 2026-05-05 — 锁粒度细化：数据库级 → 列文件级

### 变更概要

将写入锁从全局数据库级（`<data_dir>/.lock`）下沉到列文件级（`<part_dir>/<col>.col`），消除不同表、不同 Part、不同列之间的写入互斥。

### 核心原则

- 使用 `flock(LOCK_EX)` 直接锁 `.col` 数据文件，不再使用独立 `.lock` 文件
- `flock` 是 advisory lock：Reader 不调 `flock`，直接 mmap/fread，读完全不受写锁影响
- 只有两个写者同时写**同一个 .col 文件**时才会互斥

### 改动点

#### 1. `ColumnFile::Open` — 新增列文件锁

`src/storage/column_file.h:68`, `src/storage/column_file.cpp:31`

- 新增 `bool exclusive = false` 参数
- `exclusive=true` 时在 `fopen` 后立即 `flock(fileno(f), LOCK_EX)`
- `fclose` 时自动释放 flock（由 fd 生命周期保证）
- 默认 `false`，Reader 路径行为不变

#### 2. `Part::Create` — 写每列时加锁

`src/storage/part.cpp:98`

- 每列 `ColumnFile::Open(col_path, type, /*exclusive=*/true)` 
- 每列独立锁，不同列的写入不互斥
- `mkdir` 增加 EEXIST 容忍（列文件可能已预先创建目录）

#### 3. `Part::WriteColumn` — 更新列时加锁

`src/storage/part.cpp:358`

- `ColumnFile::Open(tmp_path, type, /*exclusive=*/true)`
- 锁 `.col.tmp` 文件，阻止并发更新同一列

#### 4. `Appender::WritePart` — 移除全局锁

`src/engine/appender.cpp:98`

- 移除 `FileLock::Acquire(data_dir, LOCK_EX)` 调用
- 不再需要 `data_dir` 计算
- 锁已由 `Part::Create` → `ColumnFile::Open` 内部处理

#### 5. `DoUpdateColumn` — 移除全局锁

`src/engine/connection.cpp:441`

- 移除 `FileLock::Acquire(db.path(), LOCK_EX)` 调用
- 移除 `WaveDB& db` 参数（不再需要）
- 锁已由 `Part::WriteColumn` → `ColumnFile::Open` 内部处理

#### 6. `MergeScheduler::Run` — 移除全局锁

`src/storage/merge_scheduler.cpp:88`

- 移除 `FileLock::Acquire(data_dir_, LOCK_EX)` 调用
- Merge 读 n-Part 无锁，写 m-Part 的锁在 `Part::Create` 内部

### 影响分析

| 维度 | 影响 |
|------|------|
| 并发写入 | 不同表/不同 Part/不同列写入不再互斥 |
| 读性能 | 不受影响（Reader 不调 flock） |
| 文件格式 | 不变（不引入新文件） |
| 二进制兼容 | 完全兼容 |
| 崩溃安全 | flock 关联 fd 生命周期，进程崩溃自动释放 |

### 改动汇总

| 文件 | 变化 |
|------|------|
| `column_file.h/cpp` | `Open()` 新增 `exclusive` 参数，内部 `flock(LOCK_EX)` |
| `part.cpp` | `Create` 每列传 `exclusive=true`；`WriteColumn` 传 `exclusive=true`；`mkdir` 容忍 `EEXIST` |
| `appender.cpp` | 移除 `FileLock::Acquire` |
| `connection.cpp` | `DoUpdateColumn` 移除锁、移除 `db` 参数 |
| `merge_scheduler.cpp` | 移除 `FileLock::Acquire` |
