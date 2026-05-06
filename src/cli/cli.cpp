// CLI 实现：REPL 循环、dot 命令、SQL 执行、结果输出。
//
// 使用 linenoise 实现行编辑和历史（支持 tab 补全）。

#include "src/cli/cli.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include "linenoise.h"
#include "src/parser/parser.h"
#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/schema.h"
#include "wavedb/types.h"

namespace wavedb {
namespace cli {

// 持有 WaveDB + Connection，按需创建/重建。
struct Shell::Impl {
    std::unique_ptr<WaveDB> db;
    std::unique_ptr<Connection> conn;
    std::string db_path;
};

Shell::Shell() : impl_(new Impl) {}
Shell::~Shell() { delete impl_; }

// SQL 关键字列表（用于 tab 补全）。
static const char* kSQLKeywords[] = {
    "CREATE", "TABLE", "INSERT", "INTO",   "VALUES", "SELECT", "FROM",      "WHERE", "AND",
    "LIMIT",  "ALTER", "ADD",    "DROP",   "COLUMN", "FIELD",  "TIMESTAMP", "FLOAT", "INT",
    "DAY",    "HOUR",  "MINUTE", "SECOND", "MILLI",  "MICRO",  nullptr,
};

// 全局 Shell 指针（供补全回调使用，linenoise 上游不支持 user_data）。
static Shell* g_shell = nullptr;

// 补全回调：收集关键字 + 当前数据库中的表名。
static void CompletionCallback(const char* buf, linenoiseCompletions* lc) {
    Shell* shell = g_shell;

    // 关键字
    size_t len = strlen(buf);
    for (int i = 0; kSQLKeywords[i]; ++i) {
        if (strncasecmp(kSQLKeywords[i], buf, len) == 0) linenoiseAddCompletion(lc, kSQLKeywords[i]);
    }

    // 表名
    if (shell->conn()) {
        auto tables = shell->conn()->ListTables();
        for (auto& t : tables) {
            if (strncasecmp(t.c_str(), buf, len) == 0) linenoiseAddCompletion(lc, t.c_str());
        }
    }
}

// ---- REPL ----

int Shell::Run(const std::string& data_dir) {
    std::cout << "WaveDB v0.3  (type .help for help)\n\n";

    // 设置补全 + 历史
    g_shell = this;
    linenoiseSetCompletionCallback(CompletionCallback);
    linenoiseHistorySetMaxLen(1000);
    linenoiseHistoryLoad(".wavedb_history");

    if (!data_dir.empty()) {
        if (!OpenDB(data_dir)) return 1;
    }

    while (running_) {
        std::string prompt = impl_->db ? ("wavedb:" + impl_->db_path + "> ") : "wavedb> ";
        char* raw = linenoise(prompt.c_str());
        if (!raw) {
            // Ctrl+D
            running_ = false;
            break;
        }

        std::string line(raw);
        free(raw);

        // 去掉尾部空白
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.empty()) continue;

        std::string_view sv(line);

        // 添加到历史（非空行）
        linenoiseHistoryAdd(line.c_str());

        // Dot 命令
        if (sv[0] == '.') {
            RunDotCommand(sv);
            continue;
        }

        // SQL
        RunSQL(sv);
    }

    linenoiseHistorySave(".wavedb_history");
    std::cout << "\n";
    return 0;
}

// ---- 连接管理 ----

bool Shell::OpenDB(const std::string& data_dir) {
    auto db = WaveDB::Open(data_dir);
    if (!db.ok()) {
        std::cerr << "Error: " << db.status.message() << "\n";
        return false;
    }
    impl_->db = std::make_unique<WaveDB>(std::move(*db));
    impl_->conn = std::make_unique<Connection>(*impl_->db);
    impl_->db_path = data_dir;
    return true;
}

void Shell::CloseDB() {
    impl_->conn.reset();
    impl_->db.reset();
    impl_->db_path.clear();
}

Connection* Shell::conn() const { return impl_->conn.get(); }

// ---- Dot 命令 ----

bool Shell::RunDotCommand(std::string_view line) {
    auto rest = [&](size_t pos) -> std::string_view {
        auto s = line.substr(pos);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        return s;
    };

    if (line.starts_with(".help")) {
        std::cout << R"(WaveDB CLI commands:

  Dot commands:
    .help              Show this help
    .open <path>       Open a database directory
    .close             Close current database
    .tables            List all tables
    .schema <table>    Show table schema
    .quit | .exit      Exit

  SQL (terminate with ; or newline):
    CREATE TABLE name (col TYPE, ...);
    INSERT INTO name VALUES (val, ...);
    SELECT [*|col,...] FROM name [WHERE col >= val [AND col <= val]] [LIMIT n];
    ALTER TABLE name ADD COLUMN name TYPE;
    ALTER TABLE name DROP COLUMN name;
    ALTER TABLE name ADD FIELD name TYPE;
    ALTER TABLE name DROP FIELD name;
    UPDATE name SET col = val,...;

  Types: TIMESTAMP[(DAY|HOUR|MINUTE|SECOND|MILLI|MICRO)], FLOAT, INT
  Timestamp literal: 20260101, 20260101-09:30:00, 20260101-09:30:00-123456
  Values: integers (42, -1), floats (3.14, -0.5), timestamp literals
)";
        return true;
    }

    if (line.starts_with(".open")) {
        auto path = rest(5);
        if (path.empty()) {
            std::cerr << "Usage: .open <data_dir>\n";
            return true;
        }
        CloseDB();
        OpenDB(std::string(path));
        std::cout << "opened: " << path << "\n";
        return true;
    }

    if (line.starts_with(".close")) {
        CloseDB();
        std::cout << "closed\n";
        return true;
    }

    if (line.starts_with(".tables")) {
        if (!impl_->conn) {
            std::cerr << "No database open. Use .open <path>\n";
            return true;
        }
        auto names = impl_->conn->ListTables();
        if (names.empty()) {
            std::cout << "(empty)\n";
        } else {
            for (auto& n : names) std::cout << n << "\n";
        }
        return true;
    }

    if (line.starts_with(".schema")) {
        if (!impl_->conn) {
            std::cerr << "No database open. Use .open <path>\n";
            return true;
        }
        auto arg = rest(7);
        if (arg.empty()) {
            std::cerr << "Usage: .schema <table_name>\n";
            return true;
        }
        auto* schema = impl_->conn->GetTableSchema(arg);
        if (!schema) {
            std::cerr << "Table not found: " << arg << "\n";
            return true;
        }
        std::cout << schema->ToJson() << "\n";
        return true;
    }

    if (line.starts_with(".quit") || line.starts_with(".exit")) {
        running_ = false;
        return true;
    }

    std::cerr << "Unknown command: " << line << "  (type .help for help)\n";
    return true;
}

// ---- SQL 执行 ----

bool Shell::RunSQL(std::string_view sql) {
    if (!impl_->conn) {
        std::cerr << "No database open. Use .open <path> or specify data dir on command line.\n";
        return false;
    }

    // 去掉尾部分号
    std::string sql_str(sql);
    while (!sql_str.empty() && sql_str.back() == ';') sql_str.pop_back();

    ParseCallbacks cb;

    cb.on_create_table = [this](
                             std::string_view name, const std::vector<std::string>& col_names,
                             const std::vector<ColumnType>& col_types,
                             const std::vector<TimePrecision>& col_precs,
                             MergeConfig merge_cfg) -> Status {
        TableSchema schema{std::string(name)};
        for (size_t i = 0; i < col_names.size(); ++i) schema.AddColumn(col_names[i], col_types[i], col_precs[i]);
        schema.setMergeConfig(merge_cfg);
        Status s = impl_->conn->CreateTable(schema);
        if (s.ok()) std::cout << "Table '" << name << "' created.\n";
        return s;
    };

    cb.on_insert = [this](std::string_view name, const std::vector<Value>& values) -> Status {
        Status s = impl_->conn->Insert(name, values);
        if (s.ok()) std::cout << "1 row inserted.\n";
        return s;
    };

    cb.on_select = [this](
                       std::string_view name, const std::vector<std::string>& cols, Timestamp from_ts,
                       TimePrecision /*from_prec*/, Timestamp to_ts,
                       TimePrecision /*to_prec*/, int64_t limit, std::vector<std::string>& out_col_names,
                       std::vector<ColumnType>& out_col_types, std::vector<TimePrecision>& out_col_precs,
                       std::vector<std::vector<Value>>& out_rows) -> Status {
        std::vector<std::string> select_cols = cols;
        if (select_cols.empty()) select_cols = {"*"};
        auto result = impl_->conn->Select(name, select_cols, from_ts, to_ts, limit);
        if (!result.ok()) return result.status;
        out_col_names = std::move(result->column_names);
        out_col_types = std::move(result->column_types);
        out_col_precs = std::move(result->column_precisions);
        out_rows = std::move(result->rows);
        PrintResult(out_col_names, out_col_types, out_col_precs, out_rows);
        return Status::OK();
    };

    cb.on_add_column =
        [this](std::string_view table, std::string_view col_name, ColumnType type, TimePrecision prec) -> Status {
        Status s = impl_->conn->AddColumn(table, std::string(col_name), type, prec);
        if (s.ok()) std::cout << "Column '" << col_name << "' added to '" << table << "'.\n";
        return s;
    };

    cb.on_drop_column = [this](std::string_view table, std::string_view col_name) -> Status {
        Status s = impl_->conn->DropColumn(table, col_name);
        if (s.ok()) std::cout << "Column '" << col_name << "' dropped from '" << table << "'.\n";
        return s;
    };

    cb.on_update_column = [this](std::string_view table, std::string_view col_name, Timestamp from_ts,
                                 Timestamp to_ts, const std::vector<Value>& values) -> Status {
        Status s;
        if (from_ts > 0 || to_ts > 0) {
            s = impl_->conn->UpdateColumn(table, col_name, from_ts, to_ts, values);
        } else {
            s = impl_->conn->UpdateColumn(table, col_name, values);
        }
        if (s.ok()) std::cout << "Column '" << col_name << "' updated (" << values.size() << " rows).\n";
        return s;
    };

    Status s = ParseSQL(sql_str, cb);
    if (!s.ok()) {
        std::cerr << "Error: " << s.message() << "\n";
        return false;
    }
    return true;
}

// ---- 结果输出 ----

void Shell::PrintResult(
    const std::vector<std::string>& col_names,
    const std::vector<ColumnType>& col_types,
    const std::vector<TimePrecision>& col_precs,
    const std::vector<std::vector<Value>>& rows) const {
    size_t ncols = col_names.size();
    if (ncols == 0) return;

    // 计算列宽
    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; ++c) widths[c] = col_names[c].size();

    // 格式化每行每列为字符串并计算宽度
    std::vector<std::vector<std::string>> fmt_rows(rows.size(), std::vector<std::string>(ncols));
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < ncols; ++c) {
            std::string val_str;
            if (col_types[c] == ColumnType::TIMESTAMP) {
                val_str = FormatTimestamp(std::get<int64_t>(rows[r][c]), col_precs[c]);
            } else if (col_types[c] == ColumnType::FLOAT) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", std::get<double>(rows[r][c]));
                val_str = buf;
            } else {
                val_str = std::to_string(std::get<int64_t>(rows[r][c]));
            }
            fmt_rows[r][c] = val_str;
            if (val_str.size() > widths[c]) widths[c] = val_str.size();
        }
    }

    // 分隔线
    std::string sep = "+";
    for (size_t c = 0; c < ncols; ++c) sep += std::string(widths[c] + 2, '-') + "+";

    // 表头
    std::cout << sep << "\n|";
    for (size_t c = 0; c < ncols; ++c) {
        size_t pad = widths[c] + 1 - col_names[c].size();
        std::cout << " " << col_names[c] << std::string(pad, ' ') << "|";
    }
    std::cout << "\n" << sep << "\n";

    // 数据行
    if (rows.empty()) {
        std::cout << sep << "\n0 rows\n";
        return;
    }

    for (size_t r = 0; r < rows.size(); ++r) {
        std::cout << "|";
        for (size_t c = 0; c < ncols; ++c) {
            size_t pad = widths[c] + 1 - fmt_rows[r][c].size();
            std::cout << " " << fmt_rows[r][c] << std::string(pad, ' ') << "|";
        }
        std::cout << "\n";
    }
    std::cout << sep << "\n";
    std::cout << rows.size() << " row" << (rows.size() != 1 ? "s" : "") << "\n";
}

}  // namespace cli
}  // namespace wavedb
