#pragma once

#include <string>

#include "src/common/status.h"

namespace wavedb {

struct WaveDBConfig {
    bool read_only = false;
};

class WaveDB {
  public:
    WaveDB() = default;
    ~WaveDB();

    static Result<WaveDB> Open(std::string path, WaveDBConfig config = {});

    WaveDB(WaveDB&& other) noexcept
        : path_(std::move(other.path_)), lock_fd_(other.lock_fd_), read_only_(other.read_only_) {
        other.lock_fd_ = -1;
    }

    WaveDB& operator=(WaveDB&& other) noexcept {
        if (this != &other) {
            CloseLock();
            path_ = std::move(other.path_);
            lock_fd_ = other.lock_fd_;
            read_only_ = other.read_only_;
            other.lock_fd_ = -1;
        }
        return *this;
    }

    const std::string& path() const { return path_; }
    bool read_only() const { return read_only_; }

  private:
    void CloseLock();

    std::string path_;
    int lock_fd_ = -1;
    bool read_only_ = false;
};

}  // namespace wavedb
