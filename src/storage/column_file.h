// 列文件——单列的持久化存储单元。
//
// 每个列文件存储一种类型的数据（INT/TIMESTAMP 或 FLOAT），
// 格式为裸二进制：row_count 个连续的元素，无 header/checksum/压缩。
//
// 文件操作模式：
//   "a+b" = append + read, create if needed。
//   这允许 ColumnFile 同时支持追加写入和随机读取（fseek + fread），
//   且不依赖 mmap（fread 路径更简单，适合当前非向量化扫描）。
//
// 行数推断：
//   Open 时通过 fstat 获取文件大小，除以 elem_size 反算已有行数。
//   这避免了单独的元数据文件，且 Append 后无需更新计数文件。
//   代价是不能存储空行（但 append-only 时序数据库不存在此场景）。
//
// 生命周期：
//   ColumnFile::Open(path, type) → Append × N → Flush → Close
//   析构时自动 fclose（若未显式 Close）。
//
// 未来演进：
//   - 可用 mmap 替代 fread（保持相同语义，只改 ReadAll* 实现）。
//   - 可加 snappy/zstd 压缩（需额外 block 索引）。
//   - 可加 CRC footer（需版本化文件格式）。

#pragma once

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

class ColumnFile {
  public:
    ColumnFile() = default;
    ~ColumnFile();

    ColumnFile(const ColumnFile&) = delete;
    ColumnFile& operator=(const ColumnFile&) = delete;

    // 移动语义：转移 FILE* 所有权。
    ColumnFile(ColumnFile&& other) noexcept
        : path_(std::move(other.path_)), type_(other.type_), file_(other.file_), row_count_(other.row_count_) {
        other.file_ = nullptr;
        other.row_count_ = 0;
    }

    ColumnFile& operator=(ColumnFile&& other) noexcept {
        if (this != &other) {
            Close();
            path_ = std::move(other.path_);
            type_ = other.type_;
            file_ = other.file_;
            row_count_ = other.row_count_;
            other.file_ = nullptr;
            other.row_count_ = 0;
        }
        return *this;
    }

    // 以 a+b 模式打开列文件（append + read, create if needed）。
    // exclusive=true 时对文件加 flock(LOCK_EX)。
    static Result<ColumnFile> Open(std::string path, ColumnType type, bool exclusive = false);

    // 追加数据块。类型须与文件类型匹配，否则返回 INVALID_ARGUMENT。
    Status Append(std::span<const int64_t> values);
    Status Append(std::span<const double> values);

    // 读取全部已有数据。row_count 通过文件大小/elem_size 反算。
    Result<std::vector<int64_t>> ReadAllInt64();
    Result<std::vector<double>> ReadAllFloat64();

    // 从指定行偏移读取 count 行。start + count 不得超过 row_count_。
    Result<std::vector<int64_t>> ReadRangeInt64(size_t start, size_t count);
    Result<std::vector<double>> ReadRangeFloat64(size_t start, size_t count);

    // 强制将 stdio buffer 刷盘。
    Status Flush();

    size_t row_count() const { return row_count_; }
    ColumnType type() const { return type_; }

    // 刷盘 + fclose。
    Status Close();

  private:
    Status AppendBytes(const void* data, size_t count, size_t elem_size);

    std::string path_;
    ColumnType type_ = ColumnType::INT;
    FILE* file_ = nullptr;  // "a+b" 模式，允许读写
    size_t row_count_ = 0;
};

}  // namespace wavedb
