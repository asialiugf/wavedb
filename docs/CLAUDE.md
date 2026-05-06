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
- compression
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

# Part 与 Merge 职责边界（v0.3+）

## n_ 文件生命周期（Part 类）

`Part` 类只管理 `n_` 开头的普通 Part 文件，完全不知道 `m_` 的存在。
**无 merge_offset 机制**——合并部分消费后，直接用剩余行重写 .col + 更新 meta.json。

| 职责 | 方法 |
|------|------|
| 创建 n_ Part | `Part::Create(parts_dir, schema, columns, min_ts, max_ts)` 内部自动生成 `n_YYYYMMDD_XXXXXX` 路径 |
| 按完整路径创建 | `Part::CreateWithPath(part_dir, ...)` 供 Merge 传已构造好的 m_ 路径 |
| 读取列 | `ReadColumn` / `ReadColumnRange`，直接从 0 读，无需 offset |
| 截断前 N 行 | `DiscardFirstRows(n)` 读剩余行 → 写 .col.tmp → rename → 更新 meta.json |
| 删除 n_ 目录 | `Delete()` 递归删除整个 Part 目录 |
| n_ 序号 | `NextSeq(parts_dir, date)` 读/写 `.n_seq`（格式 "日期 序号"），同天自增，跨天重置 |

## m_ 文件生命周期（PartManager）

`PartManager` 管理 `m_` 开头的合并 Part 文件：

| 职责 | 方法 |
|------|------|
| m_ 序号 | `NextMergeSeq(parts_dir, date)` 读/写 `.m_seq`（格式 "日期 序号"） |
| 合并 | `MergeParts(cfg)` 两分支（见下）→ 创建 m_ Part → 调 Part::DiscardFirstRows / Part::Delete |

n_ 和 m_ 序号完全独立，单文件 `.n_seq` / `.m_seq`，同天自增，跨天从 1 开始。

## MergeParts 合并逻辑（v0.3+ 简化版）

两个分支：

### 分支 A：有 MAX_ROWS（`merge_target_rows > 0`）

`BY_HOUR`/`BY_DAY`/`BY_MONTH` 降级为开关。从最小 n_ 往上用 `remaining` 减法凑 batch：

1. 统计所有 n_ 行总数，`total < m_target_rows` → 休眠
2. `remaining = m_target_rows`，从最小 n_ 开始：`remaining -= row_count`
3. `remaining > 0` → 当前 n_ 完全消费，删；继续下一个 n_
4. `remaining <= 0` → 当前 n_ 有多余，等 m_ 创建成功后 `DiscardFirstRows(take)` 重写剩余行
5. 创建 m_ Part → 部分消费的 n_ 重写 → 完全消费的 n_ 删除 → loop

### 分支 B：纯 policy（无 MAX_ROWS，`merge_target_rows == 0`）

按 boundary 分组，每组一次全取走（无 partial consume），每组一个 m_。

### 配置区分

- `WaveDBConfig::max_rows_per_part` → **仅 n_ Part** 写入时拆分大小（Appender 用），默认 2048
- `MergeConfig::merge_target_rows` → **m_ Part** 目标行数（MAX_ROWS N），0 = 不限制
- `MergeConfig::policy` → BY_HOUR/BY_DAY/BY_MONTH 或 NONE

## Appender 职责

- 不再管理 Part 命名（删除 `NextSeq`/`TsToDate`/`NextPartDir` 重复代码）
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
