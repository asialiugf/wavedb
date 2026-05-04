// Catalog 实现：目录扫描、表创建、schema 文件读写。
//
// 关键设计决策：
//   - 使用 POSIX dirent/mkdir/fopen 而非 C++ filesystem。
//     原因：避免 libstdc++fs 链接依赖，减少二进制大小，
//       POSIX API 行为明确（无 locale/filesystem race 问题）。
//   - Open 时损坏的 schema.json 静默跳过：
//     运维可手动删除/修复损坏表目录而不阻塞数据库启动。
//   - 表名即目录名，文件系统即索引——无额外 B-Tree/hash table。

#include "src/catalog/catalog.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace wavedb {

// ---- 路径工具 ----
// 简单的字符串拼接，不依赖 C++17 filesystem。

static std::string JoinPath(std::string_view dir, std::string_view name) {
    std::string p;
    p.reserve(dir.size() + 1 + name.size());
    p += dir;
    p += '/';
    p += name;
    return p;
}

static bool DirExists(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static Status EnsureDir(std::string_view path) {
    if (DirExists(path)) return Status::OK();
    if (::mkdir(std::string(path).c_str(), 0755) != 0)
        return Status(StatusCode::IO_ERROR, std::string("mkdir failed: ") + std::string(path));
    return Status::OK();
}

static std::string ReadFileToString(std::string_view path) {
    FILE* f = std::fopen(std::string(path).c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string content(sz, '\0');
    if (sz > 0) {
        size_t n = std::fread(content.data(), 1, sz, f);
        (void)n;
    }
    std::fclose(f);
    return content;
}

static Status WriteStringToFile(std::string_view path, std::string_view content) {
    FILE* f = std::fopen(std::string(path).c_str(), "wb");
    if (!f) return Status(StatusCode::IO_ERROR, std::string("cannot write: ") + std::string(path));
    size_t n = std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    if (n != content.size()) return Status(StatusCode::IO_ERROR, std::string("write truncated: ") + std::string(path));
    return Status::OK();
}

// ---- Catalog ----

Result<Catalog> Catalog::Open(std::string data_dir) {
    // 确保 data_dir 存在（不存在则创建空目录）
    Status s = EnsureDir(data_dir);
    if (!s.ok()) return s;

    Catalog cat;
    cat.data_dir_ = std::move(data_dir);

    // 扫描 data_dir 下所有子目录，加载 schema.json
    DIR* dp = ::opendir(cat.data_dir_.c_str());
    if (!dp) return Status(StatusCode::IO_ERROR, "cannot open data dir");

    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;  // 跳过 . .. 及隐藏目录

        std::string table_dir = JoinPath(cat.data_dir_, entry->d_name);
        if (!DirExists(table_dir)) continue;  // 跳过非目录文件（如 .lock）

        std::string schema_path = JoinPath(table_dir, "schema.json");
        std::string json = ReadFileToString(schema_path);
        if (json.empty()) continue;  // 无 schema.json → 不是表目录

        auto result = TableSchema::FromJson(json);
        if (!result.ok()) continue;  // schema 损坏 → 静默跳过

        cat.tables_.push_back(std::move(*result));
    }
    ::closedir(dp);

    return cat;
}

Status Catalog::CreateTable(const TableSchema& schema) {
    if (GetTable(schema.name()) != nullptr) return Status(StatusCode::ALREADY_EXISTS, "table exists: " + schema.name());

    // 创建表子目录
    Status s = CreateTableDir(schema.name());
    if (!s.ok()) return s;

    // 写入 schema.json
    s = WriteSchemaFile(schema);
    if (!s.ok()) return s;

    tables_.push_back(schema);
    return Status::OK();
}

const TableSchema* Catalog::GetTable(std::string_view name) const {
    for (auto& t : tables_) {
        if (t.name() == name) return &t;
    }
    return nullptr;
}

TableSchema* Catalog::GetTable(std::string_view name) {
    for (auto& t : tables_) {
        if (t.name() == name) return &t;
    }
    return nullptr;
}

Status Catalog::CreateTableDir(std::string_view table_name) {
    std::string path = JoinPath(data_dir_, table_name);
    return EnsureDir(path);
}

Status Catalog::WriteSchemaFile(const TableSchema& schema) {
    std::string path = JoinPath(JoinPath(data_dir_, schema.name()), "schema.json");
    return WriteStringToFile(path, schema.ToJson());
}

Status Catalog::AddColumn(std::string_view table_name, std::string field_name, ColumnType type, TimePrecision prec) {
    TableSchema* schema = GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));

    // 检查列名是否已存在
    if (schema->ColumnIndex(field_name) >= 0)
        return Status(StatusCode::ALREADY_EXISTS, "column already exists: " + std::string(field_name));

    schema->AddColumn(std::string(field_name), type, prec);

    // 重写 schema.json 使变更持久化
    Status s = WriteSchemaFile(*schema);
    if (!s.ok()) {
        // 回滚内存修改（schema.json 写入失败则恢复）
        schema->DropColumn(field_name);
        return s;
    }
    return Status::OK();
}

Status Catalog::DropColumn(std::string_view table_name, std::string_view field_name) {
    TableSchema* schema = GetTable(table_name);
    if (!schema) return Status(StatusCode::NOT_FOUND, "table not found: " + std::string(table_name));

    // 先保存列的完整定义，以便 schema.json 写入失败时回滚
    const ColumnDef* col_def = schema->FindColumn(field_name);
    if (!col_def) return Status(StatusCode::NOT_FOUND, "column not found: " + std::string(field_name));

    ColumnDef saved = *col_def;
    if (!schema->DropColumn(field_name))
        return Status(StatusCode::NOT_FOUND, "column not found: " + std::string(field_name));

    // 重写 schema.json 持久化删除
    Status s = WriteSchemaFile(*schema);
    if (!s.ok()) {
        // 回滚：将列重新加到相同位置
        schema->AddColumn(saved.name, saved.type, saved.precision);
        return s;
    }
    return Status::OK();
}

}  // namespace wavedb
