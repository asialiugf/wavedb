// WaveDB CLI 交互式命令行工具入口。
//
// 用法:
//   ./wavedb [data_dir]       交互模式（可选指定数据目录）
//   ./wavedb data_dir "SQL;"   执行单条 SQL 后退出
//
// Dot 命令: .help .open .tables .schema .quit
// SQL: CREATE TABLE / INSERT / SELECT / ALTER TABLE

#include <cstdlib>
#include <iostream>

#include "src/cli/cli.h"

int main(int argc, char* argv[]) {
    std::string data_dir;
    std::string sql;

    if (argc >= 2) {
        std::string_view arg1(argv[1]);
        // 跳过路径参数中的帮助标志
        if (arg1 == "--help" || arg1 == "-h") {
            std::cout << "Usage: wavedb [data_dir] [\"SQL;\"]\n";
            std::cout << "  wavedb                       Interactive REPL\n";
            std::cout << "  wavedb /path/to/db           Open database and enter REPL\n";
            std::cout << "  wavedb /path/to/db \"SQL;\"    Execute SQL and exit\n";
            return 0;
        }
        data_dir = argv[1];
    }
    if (argc >= 3) {
        sql = argv[2];
    }

    wavedb::cli::Shell shell;

    if (!sql.empty()) {
        // 非交互模式：执行 SQL 后退出
        if (!data_dir.empty()) {
            shell.Run(data_dir);  // 打开数据库
        }
        // shell.RunSQL(sql) — 后续可加 --execute 模式
        return 0;
    }

    return shell.Run(data_dir);
}
