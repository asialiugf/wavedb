# WaveDB CLI 命令行工具手册

## 启动

```bash
# 交互模式（空数据库，进入 REPL 后可 .open）
./wavedb

# 直接打开数据库进入 REPL
./wavedb /path/to/data

# 查看帮助
./wavedb --help
./wavedb -h
```

## 交互模式（REPL）

进入 REPL 后提示符为 `数据库路径> ` 或 `wavedb> `（未打开数据库时）。

```
WaveDB v0.3  (type .help for help)

wavedb> .open /data/market
opened: /data/market
/data/market> CREATE TABLE ticks (ts TIMESTAMP SECOND, price FLOAT, volume INT);
Table 'ticks' created.
/data/market> INSERT INTO ticks VALUES (20260101-09:30:00, 100.5, 1000);
1 row inserted.
/data/market> SELECT * FROM ticks;
+----------------------+-------+--------+
| ts                   | price | volume |
+----------------------+-------+--------+
| 20260101-09:30:00    | 100.5 | 1000   |
+----------------------+-------+--------+
1 row
/data/market> .quit
```

### 行编辑功能

- 左右方向键移动光标
- Backspace / Delete 删除字符
- 上下方向键浏览历史命令
- Tab 键自动补全（SQL 关键字 + 当前数据库表名）
- Ctrl+D 退出

历史记录保存在当前目录的 `.wavedb_history` 文件中（最多 1000 条）。

---

## Dot 命令

Dot 命令以 `.` 开头，用于数据库管理和会话控制。

| 命令 | 说明 |
|------|------|
| `.help` | 显示帮助信息 |
| `.open <path>` | 打开数据目录（先关闭当前数据库） |
| `.close` | 关闭当前数据库连接 |
| `.tables` | 列出所有表名 |
| `.schema <table>` | 以 JSON 格式显示表结构 |
| `.quit` 或 `.exit` | 退出 |

### 示例

```
wavedb> .open /data/db
/data/db> .tables
ticks
kbar_1m
/data/db> .schema ticks
{
  "name": "ticks",
  "columns": [
    {"name": "ts", "type": "TIMESTAMP", "precision": "SECOND"},
    {"name": "price", "type": "FLOAT"},
    {"name": "volume", "type": "INT"}
  ]
}
/data/db> .close
closed
wavedb> .quit
```

---

## SQL 语法

### CREATE TABLE

```sql
CREATE TABLE table_name (
    col_name TYPE,
    col_name TYPE(precision),
    ...
);
```

```sql
CREATE TABLE ticks (ts TIMESTAMP SECOND, price FLOAT, volume INT);
CREATE TABLE sensors (ts TIMESTAMP MICRO, temp FLOAT, rpm INT);
CREATE TABLE kbar (ts TIMESTAMP MINUTE, open FLOAT, high FLOAT, low FLOAT, close FLOAT, volume INT);
```

**类型：**

| 类型 | 说明 |
|------|------|
| `TIMESTAMP` | 时间戳（`int64_t` 微秒存储） |
| `FLOAT` | 浮点数（`double`） |
| `INT` | 整数（`int64_t`） |

**TIMESTAMP 精度：** `DAY` / `HOUR` / `MINUTE` / `SECOND` / `MILLI` / `MICRO`（默认）

### INSERT

```sql
INSERT INTO table_name VALUES (val1, val2, ...);
```

```sql
INSERT INTO ticks VALUES (20260101-09:30:00, 100.5, 1000);
INSERT INTO ticks VALUES (20260101-09:31:00, 101.0, 1500);
```

每次 INSERT 产生一个 Part 写入磁盘。批量写入建议使用 C++ API 的 `CreateAppender`。

### SELECT

```sql
SELECT [* | col1, col2, ...]
FROM table_name
[WHERE col >= timestamp [AND col <= timestamp]]
[LIMIT n];
```

```sql
-- 全表查询
SELECT * FROM ticks;

-- 指定列
SELECT ts, price FROM ticks;

-- 时间范围过滤
SELECT * FROM ticks WHERE ts >= 20260101-09:30:00;
SELECT * FROM ticks WHERE ts >= 20260101-09:30:00 AND ts <= 20260101-10:00:00;

-- 时间范围 + 列投影
SELECT price, volume FROM ticks WHERE ts >= 20260101-09:30:00 AND ts <= 20260101-10:00:00;

-- 取最后 N 行
SELECT * FROM ticks LIMIT 10;

-- 时间范围 + limit
SELECT * FROM ticks WHERE ts >= 20260101-09:30:00 LIMIT 5;
```

### ALTER TABLE

```sql
-- 添加列（COLUMN 和 FIELD 是同义词）
ALTER TABLE name ADD COLUMN col_name TYPE;
ALTER TABLE name ADD FIELD col_name TYPE(precision);

-- 删除列
ALTER TABLE name DROP COLUMN col_name;
ALTER TABLE name DROP FIELD col_name;
```

```sql
ALTER TABLE ticks ADD COLUMN bid_price FLOAT;
ALTER TABLE ticks ADD FIELD ask_price FLOAT;
ALTER TABLE ticks DROP COLUMN volume;
```

**注意：**
- 添加列不重写历史数据——旧 Part 中该列自动返回默认值（`0` 或 `0.0`）
- 删除列不删除旧 Part 中的 `.col` 文件——查询时该列不再出现
- schema.json 会即时更新并持久化

### UPDATE

```sql
UPDATE table_name SET col_name = val1, val2, ...;
```

```sql
-- 全表更新 price 列
UPDATE ticks SET price = 100.5, 101.0, 102.3;
```

更新值数量必须等于全表总行数，否则返回错误。每个 Part 的 `.col` 文件通过原子 rename 更新。

---

## 时间戳字面量

CLI 支持人类可读的时间戳格式，对应 `ParseTimestamp()` 函数：

```
20260101                      → DAY 精度
20260101-09                   → HOUR 精度
20260101-09:30                → MINUTE 精度
20260101-09:30:00             → SECOND 精度
20260101-09:30:00-123         → MILLI 精度
20260101-09:30:00-123456      → MICRO 精度
```

输入精度可低于列精度，缺失部分自动补零。

---

## 数值字面量

```sql
-- 整数
42
-1
+100

-- 浮点数（含小数点即为 FLOAT）
3.14
-0.5
100.0
```

---

## 输出格式

查询结果以 ASCII 表格形式输出：

```
+----------------------+-------+--------+
| ts                   | price | volume |
+----------------------+-------+--------+
| 20260101-09:30:00    | 100.5 | 1000   |
| 20260101-09:31:00    | 101.0 | 1500   |
+----------------------+-------+--------+
2 rows
```

- 列宽自动计算（取表头和数据最宽值）
- 时间戳列按列精度格式化显示
- 空表显示 `0 rows`

---

## 用法示例

### 写入并查询 Tick 数据

```bash
./wavedb /data/market
```

```sql
/data/market> CREATE TABLE ticks (ts TIMESTAMP SECOND, price FLOAT, volume INT);
/data/market> INSERT INTO ticks VALUES (20260101-09:30:00, 100.5, 1000);
/data/market> INSERT INTO ticks VALUES (20260101-09:30:01, 100.6, 500);
/data/market> INSERT INTO ticks VALUES (20260101-09:30:02, 100.4, 2000);
/data/market> SELECT * FROM ticks;
/data/market> .quit
```

### K 线数据管理

```bash
./wavedb /data/kbar
```

```sql
/data/kbar> CREATE TABLE kbar_1m (ts TIMESTAMP SECOND, open FLOAT, high FLOAT, low FLOAT, close FLOAT, volume INT);
/data/kbar> ALTER TABLE kbar_1m ADD COLUMN vwap FLOAT;
/data/kbar> .schema kbar_1m
/data/kbar> .quit
```
