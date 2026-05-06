# WaveDB 程序与类说明

## 程序入口

### main.cpp — 可执行文件入口

- 解析命令行参数：`--help` / `data_dir` / `SQL`
- 两种模式：
  - **交互 REPL**：`./wavedb [data_dir]`，启动 Shell 交互循环
  - **非交互执行**：`./wavedb data_dir "SQL;"`，执行单条 SQL 后退出
- 创建 `wavedb::cli::Shell` 并调用 `Run()`

---

## 模块一：common/ — 基础类型层（零依赖）

### `types.h` — 基础类型定义

| 类型 | 说明 |
|------|------|
| `enum class ColumnType` | 列数据类型：`TIMESTAMP`(0) / `FLOAT`(1) / `INT`(2)。所有列定长 8 字节，按 sizeof * row_count 直接寻址 |
| `enum class TimePrecision` | 时间戳显示精度：`DAY` / `HOUR` / `MINUTE` / `SECOND` / `MILLI` / `MICRO`。仅影响输出格式和部分解析行为，存储始终是微秒 int64_t |
| `enum class MergePolicy` | Part 合并策略：`NONE`(不合并) / `BY_HOUR`(按小时) / `BY_DAY`(按天) / `BY_MONTH`(按月) |
| `struct MergeConfig` | 合并配置：`policy`(策略) + `merge_target_rows`(m_ Part 目标行数，0=不限制) |
| `using Timestamp = int64_t` | 微秒纪元时间戳（Unix epoch µs），范围 ±292 年 |
| `using Value = variant<int64_t, double>` | 通用数据值。int64_t 同时承载 INT 和 TIMESTAMP，由 ColumnType 区分。栈对象，无堆分配，cache-friendly |

**公开函数（types.h / time_format.cpp）：**

| 函数 | 作用 |
|------|------|
| `ColumnTypeSize(ColumnType)` | 返回列值定长字节数（当前全部 8 字节） |
| `FormatTimestamp(ts, prec)` | 将微秒时间戳格式化为人类可读字符串。按精度输出不同格式（YYYYMMDD / YYYYMMDD-HH / ... / YYYYMMDD-HH:MM:SS-micro） |
| `TimePrecisionName(prec)` | 精度枚举 → 名称字符串（"DAY"/"HOUR"/...），用于 JSON schema 序列化 |
| `TimePrecisionFromName(name)` | 名称字符串 → 精度枚举，用于 JSON schema 反序列化 |
| `MergePolicyName(policy)` | 合并策略枚举 → 名称字符串（"none"/"by_hour"/"by_day"/"by_month"） |
| `MergePolicyFromName(name)` | 名称字符串 → 合并策略枚举 |
| `TruncateToPrecision(ts, prec)` | 将时间戳截断到指定精度周期起点。例如：SECOND 精度 → 丢弃亚秒；DAY 精度 → 丢弃时分秒 |
| `ExpandToPeriodEnd(ts, prec)` | 将粗精度时间戳扩展到该周期末尾。例如：DAY → ts + 一天 - 1µs = 23:59:59.999999。用于 WHERE <= 上界自适应 |
| `TimestampLiteralPrecision(s)` | 从时间戳字面量字符串推断输入精度。YYYYMMDD → DAY；有 `:` 无 sub → SECOND；有 `-` 无 `:` → HOUR 等 |
| `ParseTimestamp(str, prec)` | 将时间字符串解析为微秒时间戳。支持 YYYYMMDD[-HH[:MM[:SS[-sub]]]]。容错：输入短于列精度时缺失部分自动补零 |

### `status.h` — 错误处理基础设施

设计原则：使用 `Status` + `Result<T>` 而非 C++ exception。原因：(1) 错误路径可预测，无 hidden control flow (2) 热路径无 unwind 开销 (3) 错误码便于序列化/日志记录。

| 类型 | 说明 |
|------|------|
| `enum class StatusCode` | 错误码：`OK`(0) / `NOT_FOUND` / `ALREADY_EXISTS` / `INVALID_ARGUMENT` / `PARSE_ERROR` / `IO_ERROR` / `INTERNAL` |

**`class Status` — 操作状态**

栈对象（16 字节：code_ + string msg_），成功时无堆分配。

| 方法 | 作用 |
|------|------|
| `Status()` | 默认构造，状态为 OK |
| `Status(code, msg)` | 构造带错误码和描述的状态 |
| `static Status OK()` | 返回成功状态 |
| `ok()` | 检查是否成功（code_ == OK） |
| `code()` | 获取错误码 |
| `message()` | 获取错误描述字符串 |
| `operator bool()` | 允许 `if (!status)` 判断错误 |

**`template <typename T> struct Result` — 结果或错误**

同时承载值和错误，用法类似 Rust Result。隐式构造允许 `return 值` 或 `return Status`。

| 方法 | 作用 |
|------|------|
| `Result(T v)` | 从值隐式构造（成功结果） |
| `Result(Status s)` | 从 Status 隐式构造（错误结果） |
| `ok()` | 检查是否成功 |
| `operator bool()` | 同 ok() |
| `operator*()` / `operator->()` | 访问内部值 |

---

## 模块二：catalog/ — 元数据管理

### `class ColumnDef`（schema.h）

列定义：名称 + 类型 + 时间精度。

| 字段 | 说明 |
|------|------|
| `name` | 列名（string） |
| `type` | 列类型（ColumnType） |
| `precision` | 时间精度（TimePrecision，仅对 TIMESTAMP 有效，非 TIMESTAMP 忽略） |

### `class TableSchema`（schema.h + schema.cpp）

表结构定义。管理列定义、合并配置、JSON 序列化/反序列化。运行时 schema 视为不可变（当前不支持 ALTER TABLE 修改），这简化了 Part 的读取逻辑。

| 方法 | 作用 |
|------|------|
| `TableSchema()` | 默认构造 |
| `TableSchema(name)` | 以表名构造 |
| `name()` / `set_name(name)` | 获取/设置表名 |
| `mergeConfig()` / `setMergeConfig(cfg)` | 获取/设置合并配置 |
| `AddColumn(name, type, prec)` | 添加列。列顺序即存储顺序，不可重排（保证文件格式稳定） |
| `DropColumn(name)` | 按名称删除列。O(n) 线性扫描。旧 Part 中 .col 文件保留不删除（不重写历史），查询时该列不再出现。返回 false 表示列不存在 |
| `column_count()` | 返回到数 |
| `column_at(i)` | 按索引访问列定义 |
| `columns()` | 获取所有列定义 |
| `ColumnIndex(name)` | 按名称查找列索引。O(n) 线性扫描（n 通常 < 50）。返回 -1 表示未找到 |
| `FindColumn(name)` | 按名称查找列定义指针。返回 nullptr 表示未找到 |
| `RowByteSize()` | 行存储总字节数。当前所有列定长 8B，用于快速校验 INSERT 行大小 |
| `ToJson()` | 序列化为 JSON 字符串。格式：`{"name":"...", "columns":[{...}], "merge":{...}}`。merge 配置仅在非 NONE 时写入 |
| `static FromJson(json)` | 从 JSON 字符串反序列化。手写解析器（零外部依赖），未知字段自动跳过保证向前兼容。列定义缺少 name 或 type → PARSE_ERROR；列精度缺失 → 默认 MICRO |

### `class Catalog`（catalog.h + catalog.cpp）

目录管理器——表的注册与发现。管理 data_dir 下所有表的生命周期。

| 方法 | 作用 |
|------|------|
| `static Open(data_dir)` | 打开 data_dir，扫描子目录加载所有表的 schema.json → 重建 TableSchema。data_dir 不存在时自动创建。跳过隐藏目录（`.` 开头）、非目录文件、损坏的 schema.json（静默跳过，不阻止数据库打开） |
| `CreateTable(schema)` | 创建新表：在 data_dir 下建子目录 + 写入 schema.json + 加入内存列表。同名表存在返回 ALREADY_EXISTS |
| `AddColumn(table_name, field_name, type, prec)` | 添加列：更新内存 schema + 重写 schema.json。表不存在返回 NOT_FOUND。列名已存在返回 ALREADY_EXISTS。schema.json 写入失败时回滚内存修改 |
| `DropColumn(table_name, field_name)` | 删除列：从 schema 移除 + 重写 schema.json。列不存在返回 NOT_FOUND。旧 Part 的 .col 文件保留不删除。schema.json 写入失败时回滚内存修改（将列重新加回） |
| `GetTable(name)` | 按名称查表。O(n) 线性扫描（n = 表数，通常 < 100）。返回 nullptr 表示未找到 |
| `GetTableByIndex(i)` | 按索引访问表（用于遍历） |
| `table_count()` | 返回已加载表数 |
| `data_dir()` | 返回数据目录路径 |

**私有方法：**

| 方法 | 作用 |
|------|------|
| `CreateTableDir(table_name)` | 在 data_dir 下创建表子目录 |
| `WriteSchemaFile(schema)` | 将 schema 序列化并写入 `table_dir/schema.json` |

---

## 模块三：storage/ — 存储引擎

### `class ColumnFile`（column_file.h + column_file.cpp）

列文件——单列的持久化存储单元。每个列文件存储一种类型的数据，格式为裸二进制：row_count 个连续的元素，无 header/checksum/压缩。使用 C99 `FILE*`（"a+b" 模式 = append + read, create if needed）。

| 方法 | 作用 |
|------|------|
| `~ColumnFile()` | 析构时自动 fclose |
| `static Open(path, type, exclusive)` | 以 "a+b" 模式打开列文件。exclusive=true 时对文件加 `flock(LOCK_EX)`（用于写路径），Reader 传 false。通过 fstat 获取文件大小除以 elem_size 反算已有行数。文件不存在时自动创建 |
| `Append(values)` | 追加数据块（`span<const int64_t>` 或 `span<const double>`）。类型须与文件类型匹配，否则返回 INVALID_ARGUMENT。内部 fwrite → row_count_ 递增 |
| `ReadAllInt64()` | 读取全部数据返回 `vector<int64_t>`。rewind 到文件头 → fread 全量 |
| `ReadAllFloat64()` | 读取全部数据返回 `vector<double>` |
| `ReadRangeInt64(start, count)` | 从指定行偏移读取 count 行。fseek(start * sizeof(int64_t)) → fread count 个元素。start + count 不得超过 row_count_ |
| `ReadRangeFloat64(start, count)` | 同上，FLOAT 类型 |
| `Flush()` | 强制 fflush 将 stdio buffer 刷盘 |
| `Close()` | Flush + fclose（同时释放 flock） |
| `row_count()` | 返回当前文件行数 |
| `type()` | 返回文件列类型 |

### `class Part`（part.h + part.cpp）

不可变数据分区。仅管理 n_ 开头的普通 Part（m_ 由 PartManager 管理）。Part 是 WaveDB 的原子存储单元，每个 INSERT batch 生成一个 n_YYYYMMDD_XXXXXX 格式的 Part 目录。

**Part 目录结构：**
```
n_20260101_000001/
  meta.json    — 时间范围 + 行数（+ merge_boundary 仅纯 policy 模式）
  ts.col       — TIMESTAMP 列数据（裸 int64_t 数组）
  price.col    — FLOAT 列数据（裸 double 数组）
  volume.col   — INT 列数据（裸 int64_t 数组）
```

**工厂方法：**

| 方法 | 作用 |
|------|------|
| `static Create(parts_dir, schema, columns, min_ts, max_ts)` | 创建新的 n_ Part。parts_dir 为基目录（如 table/parts），内部自动：TsToDate(min_ts) → NextSeq(parts_dir, date) → MakePartDir → mkdir → 逐列写 .col → 最后写 meta.json（原子性标记 Part 完成）。每列传 `exclusive=true` 加 flock |
| `static CreateWithPath(part_dir, schema, columns, min_ts, max_ts)` | 以完整路径创建 Part。供 Merge 传已构造好的 m_ 路径使用。与 Create 的区别：不自生成 n_ 目录名，直接使用传入的 part_dir。内部委托 CreateImpl |
| `static Open(part_dir, schema)` | 打开已有 Part 目录，读取 meta.json 获取 min_ts / max_ts / row_count / merge_offset / merge_boundary。用 yyjson 解析 |

**属性访问：**

| 方法 | 作用 |
|------|------|
| `dir()` / `set_dir(d)` | 获取/设置 Part 目录路径 |
| `schema()` | 返回关联的表 schema |
| `min_ts()` / `max_ts()` | 获取 Part 的时间范围（微秒时间戳） |
| `row_count()` | 返回 Part 实际行数（无 offset 概念，始终是 .col 文件中的精确行数） |
| `merge_boundary()` / `set_merge_boundary(b)` | 获取/设置 merge boundary（仅纯 policy 模式使用） |

**列读写：**

| 方法 | 作用 |
|------|------|
| `ReadColumn(col_idx, type)` | 读取指定列的全部数据。缺失列（ALTER TABLE ADD 后旧 Part）返回 row_count 个默认值 |
| `ReadColumnRange(col_idx, type, start, count)` | 从指定行偏移读取 count 行。缺失列返回 count 个默认值 |
| `WriteColumn(col_name, type, values)` | 写入单列。原子 rename：.col.tmp → .col |

**Merge 相关（仅 n_ Part 使用）：**

| 方法 | 作用 |
|------|------|
| `DiscardFirstRows(n)` | 丢弃前 n 行：读剩余行 → 写 .col.tmp → rename → 更新 row_count_ + meta.json。由 MergeParts 在 m_ 创建成功后调用 |
| `Delete()` | 删除该 Part 的整个目录（`std::filesystem::remove_all`） |

**n_ 序号管理（静态工具方法）：**

| 方法 | 作用 |
|------|------|
| `static NextSeq(parts_dir, date)` | 获取下一个 n_ 序号。读单文件 `.n_seq`（格式 "日期 序号"），同天自增，跨天重置 |
| `static TsToDate(ts_us)` | 时间戳（微秒）→ YYYYMMDD 整数。使用 gmtime_r（线程安全） |

**私有方法：**

| 方法 | 作用 |
|------|------|
| `static MakePartDir(parts_dir, date, seq)` | 构造 n_ Part 目录完整路径：`<parts_dir>/n_<YYYYMMDD>_<XXXXXX>` |
| `static CreateImpl(part_dir, schema, columns, min_ts, max_ts)` | 内部实现：mkdir → 逐列调用 ColumnFile::Open(exclusive=true) → Append → Close → 最后写 meta.json（原子性标记 Part 完成，meta.json 存在 = Part 合法） |

### `class PartManager`（part_manager.h + part_manager.cpp）

Part 管理器——单表的所有 Part 集合管理（n_ + m_ 均在此）。

**PartManager 负责：**
- Open：扫描 table_dir/parts/ 下的 Part 目录，按 min_ts 排序加载
- GetPartsInRange：时间范围裁剪
- MergeParts：按合并策略合并 n_ Part 为 m_ Part，管理 m_ 的命名和序号

| 方法 | 作用 |
|------|------|
| `static Open(table_dir, schema)` | 打开表目录，扫描 parts/ 下所有子目录。以 meta.json 存在为准判别合法 Part（不依赖命名格式），损坏的 Part 静默跳过。同时加载 n_ 和 m_ 格式的 Part。按 min_ts 升序排序，使 GetPartsInRange 可提前终止 |
| `GetPartsInRange(from_ts, to_ts)` | 获取与 [from_ts, to_ts] 有交集的 Part 子集。利用 Part 按 min_ts 排序的特性线性扫描：跳过 max_ts < from_ts 的 Part，to_ts > 0 时跳过 min_ts > to_ts 的 Part。to_ts=0 表示无上界 |
| `all_parts()` | 返回所有 Part 的引用 |
| `MergeParts(cfg)` | 按合并策略合并 n_ Part 为 m_ Part。返回实际合并的 m_ Part 数量。**两分支见下方** |
| `TakeParts()` | 转移 Part 所有权（移动语义）。供 QueryResult::Impl 接管 Part 生命周期，保证查询期间 Part 不被释放 |
| `total_rows()` | 所有 Part 的总行数合计（含 n_ 和 m_） |

**MergeParts 两个分支：**

**分支 A（`merge_target_rows > 0`）：** `BY_HOUR`/`BY_DAY`/`BY_MONTH` 降级为开关。从最小 n_ 向上用 `remaining` 减法凑 batch：
1. 统计所有 n_ 剩余有效行数，`total_eff < merge_target_rows` → 休眠
2. `remaining = merge_target_rows`，从最小 n_ 开始 `remaining -= avail`
3. `remaining > 0` → 当前 n_ 完全消费，删
4. `remaining <= 0` → 当前 n_ 有多余行，`ConsumeRows(take)` 标记 offset，**不删**，break
5. 创建 m_ Part。loop 直到数据不够

**分支 B（纯 policy，`merge_target_rows == 0`）：** 按 boundary 分组，每组一次全取走（无 partial consume），每组一个 m_。

**PartManager 私有工具函数：**

| 函数 | 作用 |
|------|------|
| `NextMergeSeq(parts_dir, date)` | 获取下一个 m_ 序号。读 `<parts_dir>/.m_seq_<YYYYMMDD>` → last+1 → 写回。每天从 000001 开始 |
| `MakeMergePartDir(table_dir, date, seq)` | 构造 m_ Part 目录完整路径：`<table_dir>/parts/m_<YYYYMMDD>_<XXXXXX>` |
| `ComputeMergeBoundary(ts, policy)` | 将时间戳截断到合并策略对应的边界。仅分支 B 使用 |

### `class MergeScheduler`（merge_scheduler.h + merge_scheduler.cpp）

后台合并调度器。持有单一线程，定时扫描 data_dir 下所有表的 parts/ 目录，对设置了 MergePolicy 的表自动调用 PartManager::MergeParts()。

**线程模型：**
- 一个后台线程，通过 condition_variable 定时唤醒（默认 5 秒）或收到 Notify 提前唤醒
- 合并时读 n_ Part 无锁，写 m_ Part 的锁由 Part::Create 内部 ColumnFile::Open(exclusive=true) 处理
- Shutdown() 设置停止标志 + 通知 + join，析构时自动调用

| 方法 | 作用 |
|------|------|
| `MergeScheduler(data_dir)` | 构造函数：保存 data_dir 路径，启动后台线程 `Run()` |
| `~MergeScheduler()` | 析构时自动调用 `Shutdown()` |
| `Notify(table_name)` | 通知有新数据写入（非阻塞），将表名加入 dirty_tables 集合并唤醒后台线程提前扫描 |
| `Shutdown()` | 停止后台线程：`running_ = false` → `cv_.notify_one()` → `thread_.join()` |

**私有方法：**

| 方法 | 作用 |
|------|------|
| `Run()` | 后台主循环：等待 interval_secs_ 秒或收到 Notify → 扫描 data_dir 下所有表目录 → 读取 schema.json 检查 MergePolicy（NONE 跳过）→ 打开 PartManager → MergeParts() → 若有合并发生，将表标记为 dirty 以便下一轮再检查 |

---

## 模块四：engine/ — C++ API 实现

### `struct WaveDBConfig`（database.h）

数据库配置结构体。

| 字段 | 说明 |
|------|------|
| `read_only` | 只读模式。true 时所有写操作返回 INVALID_ARGUMENT。默认 false |
| `max_rows_per_part` | 单 Part 最大行数。Appender 按此值自动拆分 Part。默认 2048（0 视为 2048） |
| `chunk_size` | Fetch() 默认 chunk 大小。默认 2048 |

### `struct FileLock`（database.h + wavedb.cpp）

操作级文件锁。RAII 封装：构造时获取锁，析构时自动释放并关闭 fd。使用 POSIX `flock`（而非 fcntl），因为：(1) flock 关联 fd 生命周期，close 自动释放锁 (2) 语义简单 (3) 进程崩溃自动释放。

当前不采用读写锁分离：Reader 依赖 Part 不可变性，无需持锁。

| 方法 | 作用 |
|------|------|
| `FileLock()` | 默认构造（fd=-1） |
| `FileLock(fd, excl)` | 以 fd 和 exclusive 标志构造 |
| `~FileLock()` | 析构时自动 Unlock |
| `Unlock()` | 主动释放锁：`flock(fd, LOCK_UN)` + `close(fd)`。close 自动释放 flock 由 fd 生命周期保证 |
| `static Acquire(data_dir, exclusive)` | 获取锁。确保 data_dir 存在 → 创建 .lock 文件 → open → flock(LOCK_EX 或 LOCK_SH)。返回 RAII 封装的 FileLock 对象 |

### `class WaveDB`（database.h + wavedb.cpp）

数据库实例句柄。通过 `WaveDB::Open()` 创建，持有数据目录路径、读写模式、后台合并调度器。Connection 是主要的用户接口——WaveDB 自身轻量，主要作为共享状态持有者。使用 PIMPL 隐藏内部实现。

| 方法 | 作用 |
|------|------|
| `static Open(path, config)` | 打开数据目录。若目录不存在则自动创建（mkdir）。非只读模式时自动启动 MergeScheduler 后台线程 |
| `~WaveDB()` | PIMPL 析构（定义在 wavedb.cpp），自动停止 MergeScheduler |
| `path()` | 返回数据目录路径 |
| `read_only()` | 返回是否为只读模式 |
| `config()` | 返回数据库配置引用 |

### `struct Cell`（connection.h）

单元格值。对 `const Value&` 的轻量包装，提供到 `int64_t`/`double` 的隐式转换（Timestamp = int64_t），省去 `std::get`。

| 方法 | 作用 |
|------|------|
| `operator int64_t()` | 隐式转换到 int64_t（TIMESTAMP 和 INT 列用） |
| `operator double()` | 隐式转换到 double（FLOAT 列用） |

### `struct RowView`（connection.h）

单行视图。按列名/索引访问值，轻量栈对象。

| 方法 | 作用 |
|------|------|
| `operator[](col_name)` | 按列名取值，返回 Cell。O(n) 线性扫描（列数很小）。列名不存在返回 0 |
| `At(i)` | 按索引取值，返回 Cell |

### `struct ColumnChunk`（connection.h）

列优先数据块中的单列数据。INT/TIMESTAMP 使用 i64 存储，FLOAT 使用 f64 存储。

| 字段/方法 | 说明 |
|------|------|
| `i64` | `vector<int64_t>` — INT 或 TIMESTAMP 数据 |
| `f64` | `vector<double>` — FLOAT 数据 |
| `type` | 列类型（ColumnType），区分 i64 和 f64 哪个有效 |
| `size()` | 返回该列数据行数 |

### `struct Chunk`（connection.h）

列优先数据块。每次 Fetch() 返回一个 Chunk。columns[col] 存储该列在 [row_offset, row_offset + row_count) 范围内的所有行数据。

| 字段/方法 | 说明 |
|------|------|
| `column_names` | 选中列的名称 |
| `column_types` | 选中列的类型 |
| `column_precisions` | 选中列的精度（TIMESTAMP 用） |
| `columns` | `vector<ColumnChunk>` — 每列一个 ColumnChunk |
| `row_count` | 本块行数（所有列的 size() 应相等） |
| `ColumnCount()` | 返回到数 |
| `Column(i)` | 按索引访问 ColumnChunk |
| `ColumnIndex(name)` | 按名称查找列索引，O(n) |
| `i64Data(col)` | 获取 int64_t 列数据裸指针（TIMESTAMP / INT 用） |
| `f64Data(col)` | 获取 double 列数据裸指针（FLOAT 用） |

### `enum class StatementType`（connection.h）

SQL 语句类型标识：`SELECT` / `CREATE_TABLE` / `INSERT` / `ALTER_ADD_COLUMN` / `ALTER_DROP_COLUMN` / `UPDATE`。

### `struct QueryResult`（connection.h + connection.cpp）

统一查询结果（SELECT / DDL / DML 通用）。SELECT 时填充 column_*，rows 惰性物化或通过 Fetch() 逐块读取。DDL/DML 时使用 statement_type + rows_affected。PIMPL 持有流式状态。

| 字段/方法 | 说明 |
|------|------|
| `statement_type` | 原始语句类型 |
| `column_names` | 选中列的名称 |
| `column_types` | 选中列的类型 |
| `column_precisions` | 选中列的精度（TIMESTAMP 用） |
| `rows` | 行数据（mutable，惰性物化后可用） |
| `rows_affected` | 影响行数（INSERT=1, UPDATE=N, DDL/SELECT=0） |
| `RowCount()` | 返回行数。若 impl_ 存在且未物化，首次调用触发惰性全量物化 |
| `ColumnCount()` | 返回到数 |
| `Row(i)` / `Row("first")` / `Row("last")` | 按索引或标签访问行，返回 RowView |
| `Fetch()` | 列优先逐块读取。每次从磁盘读取最多 chunk_size 行，row_count==0 表示结束。DDL/DML 语句返回空 Chunk |
| `SetChunkSize(n)` | 设置 Fetch() 每次返回的最大行数（默认 2048） |
| `MaterializeRows()` | 惰性：从磁盘全量物化到 rows（mutable 语义）。循环 Fetch → 转置列优先为行优先 → 填入 rows |

### `class Connection`（connection.h + connection.cpp）

数据库连接。每个 Connection 独立持有 Catalog 快照——创建后对表的增删不可见。PIMPL 隐藏内部实现。

| 方法 | 作用 |
|------|------|
| `Connection(db)` | 构造：打开 Catalog 快照（加载失败不阻止连接创建——表列表为空） |
| `~Connection()` | 析构（PIMPL） |
| `CreateTable(schema)` | 建表。schema 需包含至少一列。已存在的表返回 ALREADY_EXISTS。只读模式返回 INVALID_ARGUMENT |
| `AddColumn(table, field_name, type, prec)` | 添加列。旧 Part 中该列自动返回默认值（0 / 0.0）。只读模式返回 INVALID_ARGUMENT |
| `DropColumn(table, field_name)` | 删除列。旧 Part 中该列的 .col 文件保留不删除（不重写历史），查询时不再返回该列。只读模式返回 INVALID_ARGUMENT |
| `UpdateColumn(table, col_name, values)` | 更新单列全量值。values 长度必须等于全表行数。内部：打开 PartManager → 获取所有 Part → 逐 Part 调用 `Part::WriteColumn` |
| `UpdateColumn(table, col_name, from_ts, to_ts, values)` | 按 ts 范围更新单列值。values 长度必须等于 [from_ts, to_ts] 内行数。内部：PartManager → GetPartsInRange → 逐 Part 调用 `Part::WriteColumn` |
| `Insert(table, row)` | 单行插入。内部创建临时 Appender → AppendRow → Close。高频写入应使用 CreateAppender() 批量写入 |
| `Select(table, columns, from_ts, to_ts, limit)` | 查询。columns 默认 {"*"} 返回所有列。from_ts=0/to_ts=0 不限制。limit=0 不限制（取尾部）。流程：Catalog 获取 schema → PartManager 加载 Part → GetPartsInRange 时间粗裁剪 → 逐 Part 逐列读取 → 行级时间细过滤 → limit 截断。读取路径不加锁——依赖 Part 不可变性 |
| `Query(sql)` | 统一 SQL 查询接口（DuckDB 风格）。接受完整 SQL 语句。内部：ParseSQL 解析 → 委托对应方法 → 封装为统一 QueryResult。解析失败返回 PARSE_ERROR |
| `CreateAppender(table)` | 创建批量写入器。高频写入时性能远优于逐行 Insert。Appender 缓冲时不持锁，仅在 Flush/Close 时持锁写入磁盘。传递 max_rows_per_part 配置 |
| `ListTables()` | 列出数据库中所有表名 |
| `GetTableSchema(name)` | 获取指定表的 schema，不存在返回 nullptr |
| `db()` | 返回关联的数据库实例引用 |

### `class Appender`（appender.h + appender.cpp）

批量写入器。设计核心：缓冲时不持锁——AppendRow 仅将数据追加到内存 buffer，零系统调用、零 I/O。只在 Flush() / Close() 写盘时才获取 LOCK_EX，写完后立即释放。

| 方法 | 作用 |
|------|------|
| `Appender(schema, table_dir, ts_col_idx, max_rows_per_part)` | 构造：初始化列优先缓冲区（buffers_.resize(列数)）。max_rows_per_part ≤ 0 时自动改为 2048 |
| `~Appender()` | 析构时自动刷盘未持久化的缓冲行（调用 WritePart） |
| `AppendRow(row)` | 追加一行。校验列数、类型（FLOAT 列必须传 double，INT/TIMESTAMP 列必须传 int64_t）→ 追踪 batch min_ts/max_ts → 列优先追加到 buffers_。纯内存操作，无 I/O |
| `AppendRow(args...)` | 变参模板便利方法：`AppendRow(ts, price, volume)` 等价于 `AppendRow({ts, price, volume})` |
| `Flush()` | 将缓冲行写入新的 Part 目录并清空缓冲区。调用 WritePart() |
| `Close()` | 等同于 Flush()。关闭后 Appender 仍可继续使用 |
| `buffered_rows()` | 返回当前缓冲区中待刷盘的行数 |
| `total_rows()` | 返回 Appender 生命周期内累计写入的总行数 |

**私有方法：**

| 方法 | 作用 |
|------|------|
| `WritePart()` | 按 max_rows_per_part_ 拆分 buffer 逐批写入 n_ Part。while offset < buffered_rows_: 取 take = min(max_rows_per_part_, 剩余) → 切片每列的 [offset, offset+take) → 调 Part::Create(parts_dir) 自动生成 n_ 路径并写入。写盘成功后清空缓冲区 |

---

## 模块五：parser/ — SQL 解析器

### `struct ParseCallbacks`（parser.h）

解析器回调接口。解析到语义动作时调用，由 CLI 或 Connection::Query 提供执行逻辑。无 AST——解析时直接通过回调执行。

| 回调 | 作用 |
|------|------|
| `on_create_table(name, col_names, col_types, col_precs, merge_config)` | CREATE TABLE 解析完成时调用 |
| `on_insert(name, values)` | INSERT 解析完成时调用 |
| `on_select(name, cols, from_ts, from_prec, to_ts, to_prec, limit, out_col_names, out_col_types, out_col_precs, out_rows)` | SELECT 解析完成时调用。from_ts=0/to_ts=0/limit=0 表示该子句未出现。回调负责填充输出参数 |
| `on_add_column(table, col_name, type, prec)` | ALTER TABLE ADD COLUMN/FIELD 解析完成时调用 |
| `on_drop_column(table, col_name)` | ALTER TABLE DROP COLUMN/FIELD 解析完成时调用 |
| `on_update_column(table, col_name, from_ts, to_ts, values)` | UPDATE 解析完成时调用。from_ts=0/to_ts=0 表示未指定范围（全表更新） |

**公开函数：**

| 函数 | 作用 |
|------|------|
| `ParseSQL(sql, cb)` | 解析一行 SQL 文本并调用相应回调。返回 Status。不支持的语法返回 PARSE_ERROR |

**Parser 内部类（不对外暴露）：**

| 类 | 作用 |
|------|------|
| `Tokenizer` | 词法分析器。按空白和标点切分输入，关键字大小写不敏感。返回 Token（TokenKind + text）。识别：关键字（CREATE/TABLE/INSERT/...）、标识符（表名/列名）、数字字面量（整数/浮点）、时间戳字面量（YYYYMMDD[-HH[:MM[:SS[-sub]]]]）、字符串、运算符（>= / <= / =）、标点（, / ( / ) / ; / *） |
| `Parser` | 递归下降解析器。入口 `Parse()` 根据首关键字分发：ParseCreate / ParseInsert / ParseSelect / ParseAlter / ParseUpdate。每个 Parse 方法按语法规则递归调用 Expect / ParseValue 等辅助方法 |

---

## 模块六：cli/ — 命令行交互工具

### `class Shell`（cli.h + cli.cpp）

命令行交互工具（DuckDB 风格 REPL）。使用 linenoise 实现行编辑、历史记录、Tab 补全。

| 方法 | 作用 |
|------|------|
| `Shell()` | 构造 Shell 实例 |
| `~Shell()` | 析构 |
| `Run(data_dir)` | 启动 REPL 主循环。设置补全回调 + 历史（`.wavedb_history` 文件最多 1000 条）。若指定 data_dir 则自动打开。循环：显示提示符 → 读取输入 → 解析执行。Ctrl+D 退出 |
| `conn()` | 返回当前 Connection 指针（供补全回调访问表名列表） |

**私有方法：**

| 方法 | 作用 |
|------|------|
| `RunDotCommand(line)` | Dot 命令分发。支持：`.help`(帮助)、`.open <path>`(打开数据库)、`.close`(关闭连接)、`.tables`(列出表)、`.schema <table>`(显示表结构)、`.quit/.exit`(退出) |
| `RunSQL(sql)` | 执行 SQL 语句。构建 ParseCallbacks 对接 Connection 方法 → 调 ParseSQL 解析执行 → 打印结果或错误 |
| `PrintResult(col_names, col_types, col_precs, rows)` | 打印查询结果。ASCII 表格格式：自动计算列宽 → 输出表头分隔线 → 逐行输出数据 → 输出行数统计 |
| `OpenDB(data_dir)` | 打开数据库：`WaveDB::Open` → 创建 Connection |
| `CloseDB()` | 关闭数据库连接 |

---

## 数据流总结

```
写入路径:
  Appender::AppendRow → 内存缓冲（列优先）
    → 达到 max_rows_per_part → 自动 WritePart
    → Flush/Close → 写剩余缓冲行
    → Part::Create(parts_dir) → 自动生成 n_YYYYMMDD_XXXXXX
    → ColumnFile::Open(exclusive=true) 每列 .col 加 flock
    → 写 meta.json + 各列 .col → ColumnFile::Close 释放 flock

查询路径:
  Connection::Select/Query
    → Catalog::GetTable → TableSchema
    → PartManager::Open → 扫描 parts/ 目录，按 min_ts 排序
    → GetPartsInRange(from_ts, to_ts) → 时间粗裁剪
    → Part::ReadColumn → 列优先读取 + 懒默认值（新增列旧Part自动补0）
    → 跨 Part 合并 + 行级时间细过滤 + limit 截断
    → QueryResult

合并路径:
  MergeScheduler::Run（后台线程定时/被 Notify 唤醒）
    → 扫描所有表 → 检查 MergePolicy
    → PartManager::Open → MergeParts(cfg)
    → 按 merge_boundary 分组 n_ Part
    → 逐组收集完整 batch → Part::CreateWithPath 创建 m_ Part
    → Part::ConsumeRows 标记 offset（部分消费）
    → Part::Delete + erase（完全消费）
```

## 程序清单

| 可执行文件/库 | 源文件 | 说明 |
|------|------|------|
| `libwavedb.a` / `libwavedb.so` | 所有 src/ 模块 | WaveDB 静态库和动态库 |
| `wavedb` | `src/main.cpp` + 所有模块 | CLI 交互工具（REPL + SQL 执行） |
| `writer` | `tools/writer/` | 写进程示例（独立 CMakeLists.txt） |
| `reader` | `tools/reader/` | 读进程示例（独立 CMakeLists.txt） |
| `test_*` | `tests/` | 单元测试（Google Test） |
