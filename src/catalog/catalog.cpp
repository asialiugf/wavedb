#include "src/catalog/catalog.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace wavedb {

// ---- 路径工具 ----

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
        return Status(StatusCode::kIOError, std::string("mkdir failed: ") + std::string(path));
    return Status::OK();
}

static std::string ReadFileToString(std::string_view path) {
    FILE* f = std::fopen(std::string(path).c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string content(sz, '\0');
    if (sz > 0) std::fread(content.data(), 1, sz, f);
    std::fclose(f);
    return content;
}

static Status WriteStringToFile(std::string_view path, std::string_view content) {
    FILE* f = std::fopen(std::string(path).c_str(), "wb");
    if (!f) return Status(StatusCode::kIOError, std::string("cannot write: ") + std::string(path));
    size_t n = std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    if (n != content.size()) return Status(StatusCode::kIOError, std::string("write truncated: ") + std::string(path));
    return Status::OK();
}

// ---- Catalog ----

Result<Catalog> Catalog::Open(std::string data_dir) {
    // 确保 data_dir 存在
    Status s = EnsureDir(data_dir);
    if (!s.ok()) return s;

    Catalog cat;
    cat.data_dir_ = std::move(data_dir);

    // 扫描 data_dir 下的子目录，加载 schema.json
    DIR* dp = ::opendir(cat.data_dir_.c_str());
    if (!dp) return Status(StatusCode::kIOError, "cannot open data dir");

    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;  // 跳过 . ..

        std::string table_dir = JoinPath(cat.data_dir_, entry->d_name);
        if (!DirExists(table_dir)) continue;

        std::string schema_path = JoinPath(table_dir, "schema.json");
        std::string json = ReadFileToString(schema_path);
        if (json.empty()) continue;  // 不是表目录，跳过

        auto result = TableSchema::FromJson(json);
        if (!result.ok()) continue;  // schema 损坏，跳过

        cat.tables_.push_back(std::move(*result));
    }
    ::closedir(dp);

    return cat;
}

Status Catalog::CreateTable(const TableSchema& schema) {
    if (GetTable(schema.name()) != nullptr) return Status(StatusCode::kAlreadyExists, "table exists: " + schema.name());

    Status s = CreateTableDir(schema.name());
    if (!s.ok()) return s;

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

}  // namespace wavedb
