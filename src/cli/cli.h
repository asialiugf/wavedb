// WaveDB 命令行交互工具（DuckDB 风格 REPL）。
//
// 支持：
//   - Dot 命令：.help .open .tables .schema .quit
//   - SQL 语句：CREATE TABLE / INSERT / SELECT / ALTER TABLE
//
// 设计：
//   CLI 持有 WaveDB + Connection，REPL 循环读取输入，
//   解析为 dot 命令或 SQL 语句，执行并打印结果。

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "wavedb/types.h"

namespace wavedb {

class WaveDB;
class Connection;

namespace cli {

// 输出模式
enum class OutputMode { kTable, kCsv };

// 命令行交互工具。
class Shell {
  public:
    Shell();
    ~Shell();

    // 启动 REPL。若指定 data_dir 则自动打开。
    int Run(const std::string& data_dir = "");

    // 供补全回调访问 Connection。
    Connection* conn() const;

  private:
    // Dot 命令分发
    bool RunDotCommand(std::string_view line);

    // 执行 SQL 语句
    bool RunSQL(std::string_view sql);

    // 打印查询结果
    void PrintResult(
        const std::vector<std::string>& col_names,
        const std::vector<ColumnType>& col_types,
        const std::vector<TimePrecision>& col_precs,
        const std::vector<std::vector<Value>>& rows) const;

    // 连接管理
    bool OpenDB(const std::string& data_dir);
    void CloseDB();

    class Impl;
    Impl* impl_ = nullptr;
    bool running_ = true;
};

}  // namespace cli
}  // namespace wavedb
