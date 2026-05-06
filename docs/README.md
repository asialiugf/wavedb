# WaveDB v0.3

使用 C++20 开发的高性能时序数据库。

> Everything is a time-series signal. 一切皆时间序列信号。

## 依赖

- C++20 编译器（GCC 13+）
- CMake 3.20+

## 构建

```bash
# 默认：编译库 + tools，安装到 ./lib ./bin
./scripts/build.sh

# 清理构建产物（./bin ./lib ./build 内所有文件）
./scripts/build.sh clear

# 编译并运行全部单元测试
./scripts/build.sh test
```

产出：

```
lib/libwavedb.a   静态库
lib/libwavedb.so  动态库
bin/writer        写进程
bin/reader        读进程
```

## 手动编译

```bash
# 1. 编译库 + 安装到 ./lib
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix .

# 2. 编译 tools
cd tools/writer && cmake -B build && cmake --build build
cd tools/reader && cmake -B build && cmake --build build
```

## 运行测试

```bash
# 一键编译+运行全部 74 个单元测试
./scripts/build.sh test

# 或手动：
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest                      # 全部 74 个测试
cd build && ctest -R "Select"          # 只跑 Select 相关
cd build && ctest -R "Storage"         # 只跑 Storage 相关
```

## 运行 writer / reader（多进程一写多读）

```bash
# 终端 1 — 写进程（每秒 2000 笔）
./bin/writer /tmp/wavedb_perf

# 终端 2 — 读进程（每 100ms 查询一次）
./bin/reader /tmp/wavedb_perf
```

Ctrl+C 停止。

## v0.3 最小示例

```cpp
#include "wavedb.h"
using namespace wavedb;

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

// 查询 — 支持投影、时间过滤、limit
auto r = conn.Select("ticks", {"price"}, from_ts, to_ts, 100);
```

## 功能清单

| 模块 | 状态 |
|------|------|
| 列式存储 + Part 机制 | ✅ |
| WaveDB / Connection / Appender C++ API | ✅ |
| 时间范围过滤 (from_ts / to_ts) | ✅ |
| LIMIT 截断 | ✅ |
| 6 级时间戳精度 (DAY ~ MICRO) | ✅ |
| FormatTimestamp / ParseTimestamp | ✅ |
| 多进程一写多读（操作级锁） | ✅ |
| 单元测试（Google Test, 74 用例） | ✅ |
| 公开头文件 + install 支持 | ✅ |
| SQL Parser | 待实现 |

## 文档

- [API 使用手册](docs/api.md)
- [架构设计](docs/architecture.md)

## 许可

MIT
