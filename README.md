# WaveDB

使用 C++20 开发的高性能时序数据库。

> Everything is a time-series signal. 一切皆时间序列信号。

## 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build .
./wavedb
```

依赖：C++20 编译器（GCC 13+）、CMake 3.20+。

## v0.1 最小示例

```cpp
auto db  = WaveDB::Open("/data/db");
Connection conn(*db);

// 建表 — 指定 ts 列精度为 SECOND
TableSchema schema("ticks");
schema.AddColumn("ts",    ColumnType::kTimestamp, TimePrecision::SECOND);
schema.AddColumn("price", ColumnType::kFloat);
schema.AddColumn("volume", ColumnType::kInt);
conn.CreateTable(schema);

// 批量写入
auto app = conn.CreateAppender("ticks");
app->AppendRow(ts, 100.5, 1000);
app->Close();

// 查询 — 时间戳自动按列精度格式化
auto r = conn.Select("ticks", {"*"}, from_ts, to_ts);
PrintResult(*r);
// ts                  | price  | volume
// 20260101-10:50:00   | 100.5  | 1000
```

## 文档

- [API 使用手册](docs/api.md)
- [架构设计](docs/architecture.md)

## v0.1 功能清单

| 模块 | 状态 |
|------|------|
| `common/` — Status, Result\<T\>, ColumnType, TimePrecision, Value | ✅ |
| `catalog/` — Schema(含precision), Catalog, JSON 持久化，启动恢复 | ✅ |
| `storage/` — ColumnFile 列文件 append + Flush + 全量 scan | ✅ |
| `engine/` — WaveDB, Connection, Appender, QueryResult | ✅ |
| 多进程一写多读 (flock) | ✅ |
| 时间范围过滤 (from_ts/to_ts) | ✅ |
| 时间戳格式化 (FormatTimestamp/ParseTimestamp) | ✅ |
| `parser/` — SQL 解析 | 待实现 |

## 许可

MIT
