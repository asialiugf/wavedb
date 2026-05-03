#include "src/engine/wavedb.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wavedb {

// ---- FileLock ----

FileLock::FileLock(FileLock&& other) noexcept
    : fd(other.fd), exclusive(other.exclusive) {
  other.fd = -1;
}

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
    ::close(fd);
    fd = -1;
  }
}

Result<FileLock> FileLock::Acquire(std::string_view data_dir, bool exclusive) {
  ::mkdir(std::string(data_dir).c_str(), 0755);

  std::string lock_path = std::string(data_dir) + "/.lock";
  int fd = ::open(lock_path.c_str(), O_CREAT | O_RDONLY, 0644);
  if (fd < 0)
    return Status(StatusCode::kIOError,
                  "cannot open lock file: " + lock_path);

  int op = exclusive ? LOCK_EX : LOCK_SH;
  if (::flock(fd, op) != 0) {
    ::close(fd);
    return Status(StatusCode::kInternal, "cannot acquire lock");
  }
  return FileLock(fd, exclusive);
}

// ---- WaveDB ----

Result<WaveDB> WaveDB::Open(std::string path, WaveDBConfig config) {
  WaveDB db;
  db.path_ = std::move(path);
  db.read_only_ = config.read_only;
  ::mkdir(db.path_.c_str(), 0755);
  return db;
}

}  // namespace wavedb
