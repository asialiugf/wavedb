# Release Notes

## 2026-05-05 — Part 命名格式：n_YYYYMMDD_XXXXXX / m_YYYYMMDD_XXXXXX

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
