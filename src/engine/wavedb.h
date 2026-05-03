#pragma once

#include <string>

#include "src/common/status.h"

namespace wavedb {

struct WaveDBConfig {
  bool read_only = false;
};

// 操作级文件锁：获取/释放。由 Connection 在读写时调用。
struct FileLock {
  int fd = -1;
  bool exclusive = false;

  FileLock() = default;
  FileLock(int fd, bool excl) : fd(fd), exclusive(excl) {}
  ~FileLock();

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;

  void Unlock();

  static Result<FileLock> Acquire(std::string_view data_dir, bool exclusive);
};

class WaveDB {
 public:
  WaveDB() = default;

  static Result<WaveDB> Open(std::string path, WaveDBConfig config = {});

  const std::string& path() const { return path_; }
  bool read_only() const { return read_only_; }

 private:
  std::string path_;
  bool read_only_ = false;
};

}  // namespace wavedb
