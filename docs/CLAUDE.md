# CLAUDE.md

# WaveDB

WaveDB 是一个使用 C++20 开发的高性能时序数据库。

本项目专门用于存储：

- 股票数据
- 期货数据
- Tick 数据
- KBar / OHLC 数据
- 工业传感器数据
- 温度/压力/振动数据
- GPU / CPU / 网络监控数据
- 一切可时间聚合（Time Aggregation）的数据

WaveDB 的核心思想：

    Everything is a time-series signal.
    一切皆时间序列信号。

数据库目标：

- 高性能 append-only 写入
- 高性能列式存储（Columnar Storage）
- 支持海量时序数据
- 支持时间窗口聚合
- 支持 KBar / Candle 聚合
- 支持 schema evolution
- 支持 mmap
- 支持向量化扫描（Vectorized Scan）

WaveDB 不是通用 OLTP 数据库。

不追求：

- PostgreSQL 复杂事务
- MySQL 行更新
- ORM
- Web 后端场景

WaveDB 专注：

- 时序数据
- 信号数据
- 高频 append
- analytical query
- append-only storage

---

# 核心功能

当前核心目标：

- CREATE TABLE
- ALTER TABLE ADD FIELD
- INSERT
- SELECT

后续目标：

- vectorized execution
- WAL
- MVCC
- distributed storage
- materialized kbar
- streaming aggregation

---

# 工程设计原则

WaveDB 是一个工程型数据库项目。

优先级：

1. 正确性
2. 文件格式稳定
3. 长期可维护性
4. 可调试性
5. 性能
6. 简洁性

禁止：

- 过度抽象
- 过度设计
- Java 风格架构
- 为“优雅”而重构

代码必须：

- 实际
- 可维护
- 可长期演进
- 易调试

本项目不是：

    “现代 C++ 炫技项目”

而是：

    “可运行十年的数据库内核项目”

---

# Claude 修改代码时必须遵守

Claude 必须：

- 最小修改
- 保持现有架构
- 保持现有命名风格
- 保持现有 ownership
- 保持现有线程模型
- 保持现有文件布局

禁止：

- 大规模重构
- 随意拆分类
- 引入复杂模板框架
- 修改 public API
- 修改 serialization layout
- 修改 WAL layout
- 修改 binary format

如果需要重大架构修改：

必须先分析：
- 影响模块
- 性能影响
- 兼容性影响
- 文件格式影响
- threading impact

之后再给出方案。

---

# 热路径（Hot Path）规则

以下属于热路径：

- INSERT
- WAL append
- column write
- scan execution
- filter execution
- aggregation
- compression
- mmap scan

热路径禁止：

- heap allocation
- std::function
- shared_ptr
- string copy
- virtual dispatch
- excessive logging
- exception

优先：

- stack object
- contiguous memory
- arena allocator
- object pool
- prefetch-friendly layout

---

# 内存管理规则

本项目强调：

- 显式 ownership
- 明确生命周期
- 可预测内存行为

避免：

- hidden allocation
- shared_ptr 泛滥
- copy-heavy design
- implicit ownership

优先：

- std::span
- string_view
- unique_ptr
- pool allocator
- preallocated buffer

禁止为了“现代化”而：
- 到处 shared_ptr
- 到处 lambda
- 到处 callback abstraction

---

# 线程模型规则

WaveDB 是多线程系统。

禁止：

- 随意增加锁
- 修改 lock ordering
- 修改 atomic memory order
- 在 hot path 使用 blocking call
- 随意修改 shutdown sequence

修改线程代码前：

Claude 必须解释：

- threading impact
- ownership impact
- shutdown behavior
- latency impact

---

# Schema Evolution 规则

WaveDB 必须支持：

    ALTER TABLE ADD FIELD

但：

禁止重写历史数据。

禁止：

- rewrite TB-scale data
- full historical rewrite

必须：

- metadata schema evolution
- lazy default value
- backward compatibility
- schema versioning

旧数据必须仍可读取。

---

# 文件格式规则

文件格式稳定性非常重要。

禁止：

- 修改历史文件 layout
- 修改 column binary layout
- 修改 WAL binary format

所有格式修改：

必须：
- versioned
- backward compatible
- documented

---

# 存储设计原则

WaveDB 使用：

- append-only
- immutable segment
- columnar storage
- partitioned storage

优先：

- 顺序 IO
- mmap
- scan-friendly layout
- analytical query

避免：

- random small write
- row-oriented storage
- tiny fragmented files

---

# 推荐目录结构

data/
    market/
        1m/
            2026-01-01/
                close.col
                volume.col
                metadata.json

或者：

data/
    sensor/
        motor_01/
            1s/
                2026-01-01/

---

# SQL Parser 规则

SQL parser 必须：

- 简单
- 可维护
- 易调试

禁止：

- PostgreSQL 级 parser
- 巨型 AST framework
- 复杂 optimizer

支持有限 SQL 即可。

---

# Class Design 规则

优先：

- 小 class
- 单一职责
- composition over inheritance

避免：

- God Object
- Singleton 泛滥
- Java 风格 interface hierarchy
- factory abuse

---

# 错误处理规则

优先：

- Status
- Result<T>
- error code

避免：

- exception-heavy flow
- hidden control flow

错误必须：

- 明确
- 可记录
- 可调试

---

# Logging 规则

日志必须：

- 轻量
- 结构化
- 避免 hot path overhead

禁止：

- 热路径 string format
- 大量 INFO log

---

# 测试规则

修改代码时：

优先增加：

- unit test
- serialization test
- concurrency test
- fuzz test
- benchmark

禁止随意删除测试。

---

# Claude 输出代码时必须：

1. 先分析架构
2. 解释影响模块
3. 解释性能影响
4. 解释线程影响
5. 解释兼容性影响
6. 给出最小 patch

禁止直接重写整个文件。

---

# WaveDB 哲学

WaveDB 的核心哲学：

    一切皆时间序列波动。

包括：

- 股票
- 温度
- 转速
- 电流
- GPU 使用率
- 网络流量
- 工业信号

所有数据：

都可以：

- append
- aggregate
- scan
- compress
- vectorize
- kbar 化

---

# 示例 SQL

CREATE TABLE ticks (
    ts TIMESTAMP,
    price FLOAT,
    volume INT
);

ALTER TABLE ticks
ADD FIELD bid_price FLOAT DEFAULT 0;

INSERT INTO ticks VALUES (...);

SELECT ts, price
FROM ticks
WHERE ts >= '2026-01-01';

---

# Part 与 Merge 职责边界（v0.4+ 渐进式）

## n_ 文件生命周期（Part 类）

`Part` 类只管理 `n_` 开头的普通 Part 文件。
**无 merge_offset 机制**——合并部分消费后，直接用剩余行重写 .col + 更新 meta.json。

| 职责 | 方法 |
|------|------|
| 创建 n_ Part | `Part::Create(parts_dir, schema, columns, min_ts, max_ts)` 自动生成 `n_YYYYMMDD_XXXXXX` |
| 创建（完整路径） | `Part::CreateWithPath(part_dir, ...)` 供 Merge 传 m_ 路径 |
| 读取列 | `ReadColumn` / `ReadColumnRange`，自动解压（v0.4+） |
| 截断前 N 行 | `DiscardFirstRows(n)` 读剩余行 → 重写 .col + meta.json |
| 删除 n_ 目录 | `Delete()` |
| n_ 序号 | `NextSeq(parts_dir, date)` 读/写 `.n_seq`，同天自增，跨天重置 |

## m_ 文件生命周期（PartManager，v0.4+ 渐进式）

`PartManager` 管理 `m_` 开头的合并 Part 文件：

| 职责 | 方法 |
|------|------|
| m_ 序号 | `NextMergeSeq(parts_dir, date)` 读/写 `.m_seq` |
| 渐进合并 | `MergeParts(cfg)`：找 in-progress m_ → 分组 n_ → 收集已有 + 新 n_ → Rewrite m_ |
| 跨边界拆分 | n_ Part 的 TS 跨越 boundary → 只读到 boundary_end，剩余 DiscardFirstRows |
| 关闭条件 | 出现下一个 boundary 的 n_ → m_ 关闭（mark_complete） |
| in-progress | m_ 可重写（每轮 Rewrite），关闭后不可变 |

## m_ Part 状态

m_ 分两种状态（meta.json 中 `"status"` 字段）：

| 状态 | 说明 |
|------|------|
| `in_progress` | 可重写，跨多次 wake 累积同 boundary 的 n_ 数据 |
| `complete` | 关闭后不可变，Reader 安全读 |

## MergeParts 渐进合并流程

### MAX_ROWS 模式（按行数，无时间概念）

```
wake:
  1. 找 in-progress m_（status="in_progress", row_count < target）
  2. remain = target - m_.row_count（无 m_ 则 remain = target）
  3. 统计 n_ 总行数，不够 target → 休眠
  4. 从最小 n_ 往上累加，remaining 减法
  5. in-progress m_ 存在:
       → AppendColumns(n_ 数据) 追加到 .col 末尾
       → 更新 meta.json（row_count, min/max_ts）
       新建:
       → Part::CreateWithPath（CreateImpl 内部 mkdir + 写 .col + meta.json）
  6. row_count >= target → m_ 标记 complete（set_in_progress(false)+PersistMeta）
  7. 删除完全消费的 n_，DiscardFirstRows 部分消费的 n_
```

### 纯 policy 模式（BY_HOUR/DAY/WEEK/MONTH）

```
wake:
  1. 找 in-progress m_
  2. 按 boundary 分组 n_（总是从 min_ts 重算 boundary）
  3. 确定 cur_boundary（in-progress m_ 的或最早 n_ 的）
  4. 收集同 boundary 的 n_ 数据:
       n_ max_ts > boundary_end → 跨边界拆分（读 TS 列找拆分点）
       全部在边界内 → to_delete；跨边界 → DiscardFirstRows(keep)
  5. 无新数据且后续 boundary 有 n_ → 关闭当前 m_ → return
  6. 有新数据:
       in-progress m_ → AppendColumns + PersistMeta（追加模式，不重写）
       新建 → CreateWithPath
  7. 有下一个 boundary → m_ 标记 complete，否则 in_progress
```

### Part 生命周期摘要

| 操作 | n_ Part | m_ Part (in-progress) | m_ Part (complete) |
|------|---------|----------------------|---------------------|
| 完全消费 | Delete() 删目录 | — | — |
| 部分消费 | DiscardFirstRows 重写保留 | — | — |
| 追加数据 | — | AppendColumns 追加，不删 | — |
| 关闭 | — | set_in_progress(false)+PersistMeta | — |
| 读取 | ReadColumn 直接读 | ReadColumn 直接读 | ReadColumn 直接读 |

### AppendColumns（追加模式）

- 打开 m_ 各 .col 文件（"a+b" 模式），追加新行到末尾
- 更新 row_count, min_ts, max_ts，重写 meta.json
- **m_ 目录不删，已有数据不重写**——增量追加，零冗余 I/O

### N_ Delete 原子性保证

Merge 删 n_ 目录时，Reader 可能在读。Linux 上：
- 已 fopen 的 fd 不受 remove_all 影响（inode 保留到 fclose）
- 尚未 fopen 的列 → ColumnFile::Open 失败 → ReadColumn 返回空向量，不报错
- 数据不丢：已合入 m_，Reader 也会从 m_ 读到完整数据

## Flush 流程

```
Appender::WritePart():
  1. 去重检查：遍历已有 Part 取 max_ts，任一缓冲行 ts <= max_ts → 清空 buffer 报错
  2. while offset < buffered_rows_:
       take = min(max_rows_per_part_, buffered_rows_ - offset)
       切片 buffers_[col][offset..offset+take]
       Part::Create(parts_dir, schema, sliced_cols, min_ts, max_ts)
         → TsToDate(min_ts) → NextSeq(.n_seq) → MakePartDir(n_YYYYMMDD_XXXXXX)
         → mkdir + 逐列 ColumnFile::Open(exclusive=true) → Append → Close
         → 写 meta.json（原子性标记：meta.json 存在 = Part 完成）
       offset += take
  3. 清空 buffer，reset min/max_ts
  4. merge_scheduler_->Notify(table_name)
```

## MergeScheduler

- 后台线程，默认 5s 定时或 WritePart 后 Notify 唤醒
- 有 dirty 表时跳过等待立即扫描
- 扫描后清理 dirty 标记

## Appender Notify

- `WritePart()` 成功后调用 `merge_scheduler_->Notify(table_name)`
- Notify 仅 `insert` + `notify_one`，不影响写入性能
- 合并异步在后台线程执行

## 列压缩（v0.4+）

`.col` 文件块式格式（FileHeader 16B + BlockIndex + 压缩 Block），每块 2048 行。通过 `MergeConfig.use_compression` 开关控制：

| 列类型 | 压缩算法 | 说明 |
|--------|---------|------|
| TIMESTAMP | DoD + zstd | Delta-of-Delta + zigzag + zstd |
| INT | DoD + zstd | 同上 |
| FLOAT | zstd | 直接 zstd 压缩原始字节 |

**压缩只发生在合成 m_ Part 时**（PartManager::MergeParts），n_ Part 保持裸写不变。渐进式 m_ 追加时采用读旧+拼接+重写策略。

FileHeader (16B): magic("WCDB") + version(1) + block_size(2048) + block_count + compression(0=NONE/1=DoD/2=ZSTD) + elem_size(8) + row_count

BlockIndex: block_count × 8B (每块的 data_offset)

旧格式兼容：无 "WCDB" magic → 按原始裸二进制读取。

SQL 控制压缩：

```sql
-- 开启压缩
CREATE TABLE ticks (ts TIMESTAMP(SECOND), price FLOAT, vol INT) COMPRESS;
CREATE TABLE ticks (...) MERGE BY DAY COMPRESS;
CREATE TABLE ticks (...) MERGE BY DAY MAX_ROWS 5000 COMPRESS;
```

API 控制：

```cpp
schema.mergeConfig().use_compression = true;
```

schema.json：

```json
{"merge": {"policy": "BY_DAY", "compress": true}}
```

## Reader 只读 m_ Part

`GetPartsInRange` 仍返回 n_+m_（UpdateColumn 等写路径用）。Reader（Select/Query/Fetch）使用 `GetMergedPartsInRange` / `TakeMergedParts`，跳过 n_。无 m_ 时返回空。

## NONE 策略：1:1 merge

无 MERGE 子句（`policy=NONE`, `merge_target_rows=0`）：每个 n_ 直转 m_，无分组、无行数限制。

## 并发安全

- **meta.json 原子写** — `.tmp` + `rename`
- **Part::Open 重试** — 3 次 @50ms
- **ReadColumn 容错** — n_ 被删返回空

## 配置区分

- `WaveDBConfig::max_rows_per_part` → **仅 n_ Part** 写入时拆分大小（Appender 用），默认 2048
- `MergeConfig::merge_target_rows` → **m_ Part** 目标行数（MAX_ROWS N），0 = 不限制
- `MergeConfig::policy` → BY_HOUR/BY_DAY/BY_WEEK/BY_MONTH 或 NONE

## Appender 职责

- `WritePart()` 调用 `Part::Create(parts_dir, ...)` 自动生成 n_ 路径
- 若缓冲行数 > max_rows_per_part_，自动拆分为多个 Part
- `WritePart()` 前检查 TS > 已有 Part 最大 TS，重复时清空 buffer 返回错误

---

# Claude 最终原则

如果不确定：

- 不要擅自重构
- 不要擅自 modernize
- 不要擅自改变 ownership
- 不要擅自改变 threading

优先：

- 保持稳定
- 保持兼容
- 最小修改
- 保持工程可维护性
