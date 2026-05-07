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

- **Append-only** — n_ Part 不可变，m_ in-progress 可追加，complete 不可变
- **Columnar** — 每列一个 `.col` 文件
- **渐进式 m_** — in-progress m_ 跨多次唤醒追加 n_ 数据，完成才关闭
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
│  WaveDB           数据库实例 + 配置持久化   │
│  Connection       连接（PIMPL）           │
│  Appender         批量写入 → 生成 n_ Part │
│  QueryResult      查询结果集              │
├──────────────────────────────────────────┤
│  catalog/         元数据管理              │
│  Catalog          内存注册表 + 目录扫描    │
│  TableSchema      表结构定义 + JSON       │
├──────────────────────────────────────────┤
│  storage/         存储引擎                │
│  ColumnFile       单列文件读写 + block 压缩 │
│  Part             n_/m_ Part 管理         │
│  PartManager      渐进式合并              │
│  MergeScheduler   后台定时/Notify 调度     │
├──────────────┬───────────────────────────┤
│  compression/│  压缩编解码                 │
│  compression.h DoD 占位（透传）           │
├──────────────┴───────────────────────────┤
│  common/     │  （零依赖基础层）           │
│  基础类型 + 时间格式化                    │
└──────────────┴───────────────────────────┘
```

---

## 数据流

### INSERT 路径（Write）

```
Appender::AppendRow(ts, price, vol)
  → 内存缓冲（列优先 buffers_[col][row]）
  → 追踪 batch_min_ts / batch_max_ts
  → 达到 max_rows_per_part_ 上限 → 触发自动 WritePart（与 Flush 频率无关）

Appender::Flush() / Close()
  → WritePart()
    → 去重检查：打开 PartManager → 遍历已有 Part 取 max_ts
      → 任一缓冲行 ts <= existing_max_ts → 清空 buffer，返回 INVALID_ARGUMENT
    → while offset < buffered_rows_:
        take = min(max_rows_per_part_, buffered_rows_ - offset)
        切片 buffers_[0..ncols][offset..offset+take]
        → Part::Create(parts_dir, schema, sliced_cols, min_ts, max_ts)
          → TsToDate(min_ts) → NextSeq(.n_seq) → MakePartDir
          → mkdir → 逐列 ColumnFile::Open(exclusive=true) → Append → Close
          → 最后写 meta.json（标记 Part 完成）
        offset += take
    → 清空 buffer, batch_min_ts/batch_max_ts 重置
    → merge_scheduler_->Notify(table_name)  // 唤醒后台合并
```

### SELECT 路径（Read）

```
Connection::Select("ticks", cols, from_ts, to_ts, limit)
  → Catalog::GetTable → TableSchema
  → PartManager::Open → 扫描 parts/ 目录，按 min_ts 排序加载快照
    → meta.json 存在 → Part::Open 加载（识 n_/m_，读 status/merge_boundary）
  → GetPartsInRange(from_ts, to_ts) → 跳过无交集 Part（时间裁剪）
  → 逐 Part 调用 Part::ReadColumn → ColumnFile::Open → fread
    → 列优先读取 + 懒默认值（ALTER TABLE ADD 后旧 Part 补 0）
    → ColumnFile::Open 失败（n_ 被 merge 删）→ 返回空（不报错，M_ 里有完整数据）
  → 跨 Part 行优先拼接 + 行级时间过滤 + limit 截断
  → QueryResult
```

### MERGE 路径（后台）

```
MergeScheduler::Run（后台线程）
  → 每 5s 定时唤醒 或 WritePart 后 Notify 唤醒
  → 有 dirty 表时跳过等待立即扫描
  → 扫描各表：读 schema.json → 检查 MergePolicy（NONE 跳过）
  → PartManager::Open → MergeParts(cfg)

MergeParts 渐进式：
  A. MAX_ROWS 模式（按行数）:
    → 找 in-progress m_（status="in_progress"）
    → remain = target - m_.row_count（无 m_ 则 remain = target）
    → 从最小 n_ 往上累加 remain 行
    → in-progress m_ 存在 → AppendColumns(n_ 数据) → 更新 meta.json
    → 新建 → CreateWithPath
    → row_count >= target → complete，否则 in_progress
    → 删完全消费的 n_，DiscardFirstRows 部分消费的 n_

  B. 纯 policy 模式（BY_HOUR/DAY/WEEK/MONTH）:
    → 找 in-progress m_
    → 按 boundary 分组 n_
    → 收集同 boundary 的 n_ 数据（跨 boundary 的 n_ 拆分）
    → in-progress m_ 存在 → AppendColumns(n_ 数据) → 更新 meta.json
    → 新建 → CreateWithPath
    → 无新数据但后续 boundary 有 n_ → 关闭当前 m_
    → 出现下一个 boundary → complete，否则 in_progress
    → 删/标记 n_

  AppendColumns（渐进追加）:
    → 打开 m_ 的各 .col 文件（"a+b" 模式）
    → 逐列追加新数据到文件末尾
    → 更新 row_count, min_ts, max_ts
    → 写 meta.json
    → 不删目录，不重写已有数据
```

---

## Part 详解

### n_ Part（Normal Part）

- INSERT 写入产生，**不可变**
- Appender 按 `max_rows_per_part` 自动拆分
- 合并后：**完全消费 → Delete() 删目录；部分消费 → DiscardFirstRows 重写保留**

### m_ Part（Merged Part）

- MergeScheduler 后台合并产生
- **in-progress**：**AppendColumns 追加不删**——跨多次唤醒累积同 boundary 的 n_ 数据
- **complete**：关闭后不可变，Reader 安全读取
- meta.json 中 `"status": "in_progress"` 标记状态，无此字段即 complete
- **m_ 目录从不删除**——只追加，不重写已有数据

### 跨 boundary n_ 拆分

n_ Part 的 TS 可能跨越两个 boundary（如 23:50~00:20 跨两天）。合并时：
1. 读 TS 列，找到第一个 > boundary_end 的行索引
2. 只取 boundary 内的行加入 m_
3. `DiscardFirstRows(keep)` 把剩余行留在 n_，merge_boundary 重置为 0

---

## 配置

| 配置 | 位置 | 作用 |
|------|------|------|
| `max_rows_per_part` | `WaveDBConfig` / `config.json` | n_ Part 写入拆分大小 |
| `chunk_size` | `WaveDBConfig` / `config.json` | Fetch() chunk 大小 |
| `read_only` | `WaveDBConfig` / `config.json` | 只读模式 |
| `policy` | `MergeConfig` / schema.json | 合并策略 (BY_HOUR/DAY/WEEK/MONTH) |
| `merge_target_rows` | `MergeConfig` / schema.json | m_ Part 目标行数 (MAX_ROWS) |

---

## 存储格式

### 目录结构

```
data/<db_name>/
  config.json                  数据库配置（持久化）
  <table_name>/
    schema.json                   表结构 + merge 配置
    parts/
      .n_seq                       n_ 序号（格式 "20260511 15"）
      .m_seq                       m_ 序号（格式 "20260511 3"）
      n_20260511_000001/           普通 Part（INSERT 产生）
        meta.json                  时间范围 + 行数 + status + merge_boundary
        ts.col                     列文件
        price.col
      m_20260511_000001/           合并 Part（merge 产生）
        meta.json
        ts.col
        price.col
```

### meta.json

```json
{
    "min_ts": 1778336456000000,
    "max_ts": 1778372455999999,
    "row_count": 86400,
    "merge_boundary": 1778336400000000,
    "status": "in_progress"
}
```

### .col 文件格式

**v0.3（当前）：** 裸二进制定长排列，行数 = file_size / elem_size

**v0.4+ 块式压缩：**

```
┌─────────────────────┐
│ FileHeader (16B)    │  magic="WCDB", version=1, block_size=2048,
│                     │  block_count, compression(0=none/1=DoD),
│                     │  elem_size(8), row_count
├─────────────────────┤
│ BlockIndex           │  block_count × 8B (每块 data_offset)
├─────────────────────┤
│ Block 0 data         │  变长（压缩后）
├─────────────────────┤
│ Block 1 data         │
├─────────────────────┤
│ ...                  │
└─────────────────────┘
```

**FileHeader (16 bytes):**

| offset | size | field | 说明 |
|--------|------|-------|------|
| 0 | 4 | magic | "WCDB" |
| 4 | 2 | version | 1 |
| 6 | 2 | block_size | 每块最大行数（默认 2048） |
| 8 | 2 | block_count | 已有块数 |
| 10 | 1 | compression | 0=none, 1=DoD |
| 11 | 1 | elem_size | 每元素字节数（8） |
| 12 | 4 | row_count | 总行数 |

**BlockIndex：** block_count × 8B，每块 data_offset。行数由 row_count + block_size + 块号推导，不需存 orig_size。

**DoD 压缩（TIMESTAMP 列）：**

```
Block 内布局:
  [base_value: 8B]      ← t0
  [first_delta: 8B]     ← t1 - t0
  [dod_count: 2B]       ← DoD 值个数
  [bit_width: 1B]       ← 编码位宽
  [dod_data: ...]       ← BitPacked DoD 值

原始:    t0, t1, t2, ..., tn
delta:   t1-t0, t2-t1, ..., tn-t(n-1)
DoD:     delta[0], delta[1]-delta[0], ..., delta[n-1]-delta[n-2]
```

**v0.4 阶段：** 空压缩函数（CompressBlock/DecompressBlock 透传），后续实现 DoD。

**兼容性：** Open 时读前 4 字节 → "WCDB" → 块式格式；否则 → 旧裸二进制。

**渐进追加：** in-progress m_ AppendColumns 时已有 block 不复压，只加新 block 或重写最后不满块。

---

## 项目结构

```
wavedb/
  include/
    wavedb.h             顶层公开头文件
    wavedb/              模块头文件
  src/
    common/              基础类型 + 时间格式化
    catalog/             元数据管理
    storage/             存储引擎（column_file + part + part_manager + merge_scheduler）
    engine/              API 实现（wavedb + connection + appender）
    parser/              SQL 解析器
    compression/         压缩编解码（DoD 占位）
    cli/                 命令行交互工具
    main.cpp             入口
  tests/                 单元测试（121 用例）
  tools/
    writer/              写进程
    reader/              读进程
  docs/                  文档
  third_party/           yyjson + linenoise + googletest
```
