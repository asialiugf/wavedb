// WaveDB 数据库实例与文件级锁。
//
// 线程模型：
//   WaveDB 本身不管理并发——所有并发控制由 Connection 层负责。
//   多线程通过各自的 Connection 访问同一个 WaveDB 是安全的。
//
// FileLock 设计：
//   使用 POSIX flock（而非 fcntl），原因是：
//     (1) flock 关联 fd 生命周期，close 自动释放锁，避免死锁残留
//     (2) flock 语义简单，不支持递归锁（此处不需要）
//     (3) 操作级锁：仅在写 Part 时持有 LOCK_EX，读路径不持锁
//
//   非读写锁模式：Reader 依赖 Part 不可变性，直接 mmap/fread 即可，
//   无需获取 LOCK_SH——因为已提交 Part 不会被修改或删除。
//   这与 LevelDB/RocksDB 的 SST file 语义一致。

#pragma once

#include <memory>
#include <string>

#include "wavedb/status.h"

namespace wavedb {

// 数据库配置。
struct WaveDBConfig {
    bool read_only = false;
    int64_t max_rows_per_part = 2048; // 单 Part 最大行数，0 = 默认 2048
    size_t chunk_size = 2048;         // Fetch() 默认 chunk 大小
};

// 操作级文件锁。
// RAII 封装：构造时获取锁，析构时自动释放并关闭 fd。
// 移动语义支持在调用栈中传递锁对象的所有权。
//
// 生命周期：
//   lock = FileLock::Acquire(path, exclusive=true)
//   // ... 写入 Part ...
//   lock.Unlock() 或 lock 析构时自动释放
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

    // 主动释放锁并关闭 fd。
    void Unlock();

    // 获取锁。若 data_dir 不存在则自动创建。
    static Result<FileLock> Acquire(std::string_view data_dir, bool exclusive);
};

// WaveDB 数据库句柄。
// 通过 WaveDB::Open() 创建，持有数据目录路径、读写模式、后台合并调度器。
// Connection 是主要的用户接口——WaveDB 自身轻量，主要作为共享状态持有者。
class WaveDB {
  public:
    WaveDB() = default;
    ~WaveDB();                                    // PIMPL 析构，定义在 wavedb.cpp
    WaveDB(WaveDB&&) noexcept;                    // PIMPL 移动
    WaveDB& operator=(WaveDB&&) noexcept;

    // 打开数据目录。若目录不存在则自动创建。
    // read_only=true 时，Connection 的所有写操作返回 INVALID_ARGUMENT。
    static Result<WaveDB> Open(std::string path, WaveDBConfig config = {});

    const std::string& path() const { return path_; }
    bool read_only() const { return read_only_; }
    const WaveDBConfig& config() const { return config_; }

  private:
    friend class Connection;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    bool read_only_ = false;
    WaveDBConfig config_;
};

}  // namespace wavedb
