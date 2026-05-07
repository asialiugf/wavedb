# Release Notes

## 2026-05-07 — 块式压缩实现（DoD + zstd）

### 压缩算法（使用开源 zstd）

`.col` 文件支持块式压缩格式（FileHeader 16B + BlockIndex + 压缩 Block），每块 2048 行：

| 列类型 | 压缩 | 算法 |
|--------|------|------|
| TIMESTAMP | DoD + zstd | Delta-of-Delta → zigzag 编码 → zstd |
| INT | DoD + zstd | 同上 |
| FLOAT | zstd | zstd 直接压缩原始字节 |

### 使用方式

```sql
-- 独立压缩
CREATE TABLE ticks (ts TIMESTAMP, price FLOAT, vol INT) COMPRESS;
-- MERGE + 压缩
CREATE TABLE ticks (...) MERGE BY DAY MAX_ROWS 5000 COMPRESS;
```

API：`schema.mergeConfig().use_compression = true;`（默认 `false`，不压缩）

### 压缩只发生在合成 m_ Part 时（MergeParts），n_ 保持裸写。渐进式 m_ 追加时自动读旧+拼接+重写。

### 新增文件
- `src/compression/compression.cpp` — DoD/ZSTD/NONE 压缩实现
- `src/compression/compression.h` — 压缩 API + `CompressionType` 枚举

### 修改文件
- `include/wavedb/types.h` — `MergeConfig` 新增 `use_compression`（默认 false）
- `src/storage/column_file.cpp` — `CreateBlocked` 预分配 8KB index 空间、`AppendBlocks` 渐进追加、`ReadBlockData` 末块修复
- `src/storage/part.h/cpp` — 新增 `CreateBlocked`、`AppendColumnsBlocked`
- `src/storage/part_manager.cpp` — 三合并分支均检查 `use_compression`
- `src/parser/parser.cpp` — SQL `COMPRESS` 关键字支持
- `src/catalog/schema.cpp` — `use_compression` 序列化/反序列化
- `CMakeLists.txt` — 链接 `zstd`

### 测试
- 压缩纯函数测试（DoD 往返、NONE 往返、边界情况）
- Part::CreateBlocked → Open → ReadColumn 端到端
- 渐进追加测试
- 全部 131 测试通过

---

## 2026-05-07 — Reader 只读 m_ + NONE=1:1 + meta.json 原子写

### Reader 只读 m_ Part
- `GetMergedPartsInRange` / `TakeMergedParts` — Reader 跳过 n_，只读 m_
- UpdateColumn 等写路径保留 `GetPartsInRange`（读 n_+m_）
- 无 m_ 时返回空数据

### NONE 策略：1:1 merge
- `MERGE` 不写或 `policy=NONE` → 每个 n_ 直转 m_，无分组

### BY_WEEK 合并策略
- `MERGE BY WEEK` — 按周一~周日分组
- `ComputeMergeBoundary` 用 gmtime + tm_wday 算周一零点

### 并发安全
- **meta.json 原子写** — `.tmp` + `rename`，Reader 不读半写文件
- **Part::Open 重试** — 3 次 @50ms，容忍 rename 瞬间
- **ReadColumn 容错** — n_ 被 merge 删返回空
- **AppendColumns** — in-progress m_ 追加不删，不重写已有数据

### 配置持久化
- `config.json` — WaveDB::Open 自动读写 data_dir 根目录

---

## 2026-05-07 — 渐进式 m_ Part + Notify + 列压缩框架

### 渐进式 m_ 合并
- 同一个 m_ 跨多次唤醒累积同 boundary 的 n_，直到 boundary 完成才关闭
- meta.json 新增 `"status": "in_progress"` / 省略表示 complete
- 跨边界 n_ 自动拆分（`DiscardFirstRows` 后 `merge_boundary` 重置）
- 出现下一个 boundary 时自动关闭当前 m_

### MergeScheduler Notify
- `Appender::WritePart()` 成功后 → `MergeScheduler::Notify(table_name)`
- 有 dirty 表时 MergeScheduler 跳过定时等待立即扫描
- Notify 微秒级，不影响写入性能

### 列压缩框架
- `src/compression/compression.h` — `CompressBlock`/`DecompressBlock` 空函数透传
- `.col` 新格式：`ColFileHeader`(16B) + `BlockIndex`(N×8B) + Block data
- 旧格式兼容：无 `"WCDB"` magic → 按裸二进制读取

### 其他
- `>` `<` WHERE 操作符支持，非法字符检测
- 序号文件简化为单文件 `.n_seq` / `.m_seq`（格式 "日期 序号"）
- 全部测试统一为 `wavedb_test` 可执行文件（121 用例）
- `MergeConfig::merge_target_rows` 与 `WaveDBConfig::max_rows_per_part` 明确区分

---

## 2026-05-06 — MergeParts 简化 + merge_offset 移除 + 配置重命名

### merge_offset 移除

- 删除 `merge_offset_`、`ConsumeRows()`、`effective_row_count()`
- 新增 `Part::DiscardFirstRows(n)` — 部分消费后用剩余行重写 .col + 更新 meta.json
- `ReadColumn` / `ReadColumnRange` 不再需要加 offset，直接从 0 读
- `Part::Open` 兼容旧 meta.json 的 `merge_offset` 字段（读到后 row_count 相减）
- 新 meta.json 不再写入 `merge_offset`

### MergeParts 重写

- **分支 A（有 MAX_ROWS）：** `remaining = merge_target_rows` 减法。从最小 n_ 往上累加，`remaining <= 0` 时当前 n_ 有多余行 → `ConsumeRows(take)` 标记 offset → break。完全消费的 n_ 删除。loop 直到数据不够 → 休眠。
- **分支 B（纯 policy）：** 按 boundary（BY_HOUR/BY_DAY/BY_MONTH）分组，每组一次全取走，一组一个 m_。
- 删除 `wait_more`、`group_eff`、`consumed[]`、`partials[]` 等复杂追踪。

### 配置区分

- `MergeConfig::max_rows_per_part` → `MergeConfig::merge_target_rows`，与 `WaveDBConfig::max_rows_per_part`（仅管 n_ Part 拆分）明确区分。
- JSON key: `"max_rows_per_part"` → `"merge_target_rows"`。

### Appender TS 去重

- `WritePart()` 时检查缓冲行 TS 是否大于已有 Part 的 max_ts，TS 重复返回错误。

---

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
- n_ 命名和序号完全由 `Part` 类管理（`Part::Create(parts_dir)` 自动生成路径），持久化在 `.n_seq_<YYYYMMDD>` 文件中
- m_ 命名和序号由 `PartManager` 管理（`NextMergeSeq`），持久化在 `.m_seq_<YYYYMMDD>` 文件中
- n_ 和 m_ 序号完全独立，各自每天从 000001 开始
- m-Part 的日期取自 merge boundary 对应日期

### 改动汇总

| 文件 | 变化 |
|------|------|
| `part.h/.cpp` | 新增 `Create(parts_dir)` 自动生成 n_ 路径、`CreateWithPath`（供 merge）、`Delete`（删除 n_ 目录）、`NextSeq`（持久化计数器）、`TsToDate`、`MakePartDir`；`ConsumeRows` 保持不变 |
| `part_manager.h/.cpp` | 删除 `CreatePart`/`NextPartDir`/`next_part_id_` 死代码、删除重复的 `TsToDate`/`ParsePartSeq`/`NextSeq`/`MakePartDir`；新增 `NextMergeSeq` 管理 m_ 序号（`.m_seq_<YYYYMMDD>`）；MergeParts 用 `Part::Delete` 和 `Part::CreateWithPath` |
| `appender.h/.cpp` | 删除 `TsToDate`/`NextSeq`/`NextPartDir` 重复代码；`WritePart` 调用 `Part::Create(parts_dir)`；按 `max_rows_per_part_` 自动拆分多 Part |
| `database.h` | `max_rows_per_part` 默认改为 2048 |
| `connection.cpp` | `CreateAppender`/`Insert` 传递 `max_rows_per_part` 到 Appender |

---

## 2026-05-05 — Part 大小可配置 + Flush 与 Part 边界解耦

### 变更概要

- `WaveDBConfig` 新增 `max_rows_per_part` 和 `chunk_size` 两个全局配置项
- `Appender` 在 `AppendRow` 内部按 `max_rows_per_part` 自动切分 Part，不再依赖 `Flush()` 频率
- `Flush()` 职责回归纯粹：将缓冲数据落盘（可能生成小于上限的 Part）
- 表级 `MERGE BY HOUR MAX_ROWS N` 优先于全局 config

### 改动点

1. `WaveDBConfig` 新增 `max_rows_per_part`（0=默认 2048）和 `chunk_size`（默认 2048）
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
