#include "src/engine/wavedb.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wavedb {

WaveDB::~WaveDB() { CloseLock(); }

void WaveDB::CloseLock() {
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
}

static Result<int> AcquireLock(const std::string& path, bool exclusive) {
    // 确保数据目录存在
    ::mkdir(path.c_str(), 0755);

    std::string lock_path = path + "/.lock";
    int fd = ::open(lock_path.c_str(), O_CREAT | O_RDONLY, 0644);
    if (fd < 0) return Status(StatusCode::kIOError, "cannot open lock file: " + lock_path);

    int op = exclusive ? (LOCK_EX | LOCK_NB) : LOCK_SH;
    if (::flock(fd, op) != 0) {
        ::close(fd);
        if (exclusive) return Status(StatusCode::kInternal, "database locked by another writer");
        return Status(StatusCode::kInternal, "cannot acquire read lock");
    }
    return fd;
}

Result<WaveDB> WaveDB::Open(std::string path, WaveDBConfig config) {
    auto fd = AcquireLock(path, /*exclusive=*/!config.read_only);
    if (!fd.ok()) return fd.status;

    WaveDB db;
    db.path_ = std::move(path);
    db.lock_fd_ = *fd;
    db.read_only_ = config.read_only;
    return db;
}

}  // namespace wavedb
