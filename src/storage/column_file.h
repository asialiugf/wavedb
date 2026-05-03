#pragma once

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "src/common/status.h"
#include "src/common/types.h"

namespace wavedb {

class ColumnFile {
  public:
    ColumnFile() = default;
    ~ColumnFile();

    ColumnFile(const ColumnFile&) = delete;
    ColumnFile& operator=(const ColumnFile&) = delete;

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

    // 打开已有文件，或创建新文件（若不存在）
    static Result<ColumnFile> Open(std::string path, ColumnType type);

    Status Append(std::span<const int64_t> values);
    Status Append(std::span<const double> values);

    Result<std::vector<int64_t>> ReadAllInt64();
    Result<std::vector<double>> ReadAllFloat64();

    // 强制刷盘（通常在 Close 前调用）。
    Status Flush();

    size_t row_count() const { return row_count_; }
    ColumnType type() const { return type_; }
    Status Close();

  private:
    Status AppendBytes(const void* data, size_t count, size_t elem_size);

    std::string path_;
    ColumnType type_ = ColumnType::kInt;
    FILE* file_ = nullptr;
    size_t row_count_ = 0;
};

}  // namespace wavedb
