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

WaveDB::~WaveDB() = default;
WaveDB::WaveDB(WaveDB&&) noexcept = default;
WaveDB& WaveDB::operator=(WaveDB&&) noexcept = default;

Result<WaveDB> WaveDB::Open(std::string path, WaveDBConfig config) {
    WaveDB db;
    db.path_ = std::move(path);
    db.read_only_ = config.read_only;
    ::mkdir(db.path_.c_str(), 0755);
    db.impl_ = std::make_unique<WaveDB::Impl>();
    if (!config.read_only) {
        db.impl_->merge_scheduler = std::make_unique<MergeScheduler>(db.path_);
    }
    return db;
}

}  // namespace wavedb
