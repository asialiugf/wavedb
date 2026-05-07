// WaveDB 实例与文件锁实现。
//
// FileLock 使用 Linux flock(2) 系统调用：
//   - 基于 fd 而非路径名（fnctl 基于 (pid, inode) 对），
//     因此 close(fd) 自动释放锁，避免进程崩溃后残留锁文件。
//   - LOCK_EX 用于写操作（Appender::WritePart），
//     确保同一时刻只有一个进程写入 Part。
//   - LOCK_SH 预留用于未来多 Reader 场景。
//
// 当前不采用读写锁分离：
//   Reader 依赖 Part 不可变性，无需持锁。
//   若未来支持 DROP TABLE / Compaction，Reader 需要 LOCK_SH 保护。

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wavedb/database.h"

#include "src/storage/merge_scheduler.h"
#include "third_party/yyjson.h"

namespace wavedb {

// ---- FileLock ----

FileLock::FileLock(FileLock&& other) noexcept : fd(other.fd), exclusive(other.exclusive) { other.fd = -1; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        Unlock();
        fd = other.fd;
        exclusive = other.exclusive;
        other.fd = -1;
    }
    return *this;
}

FileLock::~FileLock() { Unlock(); }

void FileLock::Unlock() {
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);  // close 自动释放 flock
        fd = -1;
    }
}

Result<FileLock> FileLock::Acquire(std::string_view data_dir, bool exclusive) {
    // 确保 data_dir 存在
    ::mkdir(std::string(data_dir).c_str(), 0755);

    // 使用独立 .lock 文件，避免锁住数据目录本身
    std::string lock_path = std::string(data_dir) + "/.lock";
    int fd = ::open(lock_path.c_str(), O_CREAT | O_RDONLY, 0644);
    if (fd < 0) return Status(StatusCode::IO_ERROR, "cannot open lock file: " + lock_path);

    int op = exclusive ? LOCK_EX : LOCK_SH;
    if (::flock(fd, op) != 0) {
        ::close(fd);
        return Status(StatusCode::INTERNAL, "cannot acquire lock");
    }
    return FileLock(fd, exclusive);
}

// ---- WaveDB ----

struct WaveDB::Impl {
    std::unique_ptr<MergeScheduler> merge_scheduler;
};

MergeScheduler* WaveDB::merge_scheduler() const { return impl_ ? impl_->merge_scheduler.get() : nullptr; }

WaveDB::~WaveDB() = default;
WaveDB::WaveDB(WaveDB&&) noexcept = default;
WaveDB& WaveDB::operator=(WaveDB&&) noexcept = default;

// 读取/写入 config.json（数据目录级配置持久化）
static std::string ReadFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string content(sz, '\0');
    if (sz > 0) { size_t n = std::fread(content.data(), 1, sz, f); (void)n; }
    std::fclose(f);
    return content;
}

static void WriteFile(const std::string& path, std::string_view content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

static std::string ConfigPath(std::string_view data_dir) {
    return std::string(data_dir) + "/config.json";
}

static WaveDBConfig ReadConfig(std::string_view data_dir) {
    std::string cfg_path = ConfigPath(data_dir);
    std::string json = ReadFile(cfg_path);
    if (json.empty()) return {};
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return {};
    yyjson_val* root = yyjson_doc_get_root(doc);
    WaveDBConfig cfg;
    if (root && yyjson_is_obj(root)) {
        yyjson_val* v = yyjson_obj_get(root, "max_rows_per_part");
        if (v) cfg.max_rows_per_part = yyjson_get_sint(v);
        v = yyjson_obj_get(root, "chunk_size");
        if (v) cfg.chunk_size = static_cast<size_t>(yyjson_get_sint(v));
        v = yyjson_obj_get(root, "read_only");
        if (v) cfg.read_only = yyjson_get_bool(v);
    }
    yyjson_doc_free(doc);
    return cfg;
}

static void WriteConfig(std::string_view data_dir, const WaveDBConfig& cfg) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "max_rows_per_part", cfg.max_rows_per_part);
    yyjson_mut_obj_add_int(doc, root, "chunk_size", static_cast<int64_t>(cfg.chunk_size));
    yyjson_mut_obj_add_bool(doc, root, "read_only", cfg.read_only);
    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    WriteFile(ConfigPath(data_dir), json_str);
    free(json_str);
    yyjson_mut_doc_free(doc);
}

Result<WaveDB> WaveDB::Open(std::string path, WaveDBConfig config) {
    WaveDB db;
    db.path_ = path;
    ::mkdir(db.path_.c_str(), 0755);

    // 读取已有配置，如果传入 config 没覆盖则沿用
    WaveDBConfig existing = ReadConfig(db.path_);
    if (config.max_rows_per_part > 0) existing.max_rows_per_part = config.max_rows_per_part;
    if (config.chunk_size != 2048) existing.chunk_size = config.chunk_size;
    if (config.read_only) existing.read_only = true;
    db.config_ = existing.max_rows_per_part > 0 ? existing : WaveDBConfig{};
    db.read_only_ = config.read_only;

    // 持久化配置
    if (!db.read_only_) WriteConfig(db.path_, db.config_);
    db.impl_ = std::make_unique<WaveDB::Impl>();
    if (!db.read_only_) {
        db.impl_->merge_scheduler = std::make_unique<MergeScheduler>(db.path_);
    }
    return db;
}

}  // namespace wavedb
