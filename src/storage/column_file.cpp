// ColumnFile 实现：定长裸二进制列文件的读写。
//
// 文件格式：
//   无 header、无 footer、无 magic number。
//   仅包含 row_count 个连续元素（int64_t 或 double）。
//   文件大小 = row_count × sizeof(element)。
//
// 为什么用 stdio FILE* 而非 mmap：
//   v0.2 以简单性优先。stdio 提供缓冲写入（fwrite → fflush），
//   且跨平台行为一致。mmap 路径将后续版本添加。
//
// "a+b" 模式的优势：
//   - 文件不存在时自动创建（O_CREAT）
//   - 写入自动追加到文件尾（O_APPEND 语义，但 "a+b" 允许 seek）
//   - 允许回退到文件头读取（rewind + fread）

#include "src/storage/column_file.h"

#include <sys/stat.h>

#include <cstring>

namespace wavedb {

ColumnFile::~ColumnFile() {
    if (file_) {
        std::fclose(file_);
    }
}

Result<ColumnFile> ColumnFile::Open(std::string path, ColumnType type) {
    // "a+b": append + binary read, create if needed
    FILE* f = std::fopen(path.c_str(), "a+b");
    if (!f) return Status(StatusCode::IO_ERROR, "cannot open: " + path);

    // 从文件大小反算已有行数（文件大小 / 元素大小）
    struct stat st;
    size_t row_count = 0;
    if (::stat(path.c_str(), &st) == 0) {
        size_t elem = ColumnTypeSize(type);
        row_count = st.st_size / elem;
    }

    ColumnFile cf;
    cf.path_ = std::move(path);
    cf.type_ = type;
    cf.file_ = f;
    cf.row_count_ = row_count;
    return cf;
}

Status ColumnFile::Flush() {
    if (file_ && std::fflush(file_) != 0) return Status(StatusCode::IO_ERROR, "flush failed: " + path_);
    return Status::OK();
}

Status ColumnFile::Close() {
    if (file_) {
        Status s = Flush();
        if (std::fclose(file_) != 0) return Status(StatusCode::IO_ERROR, "close failed: " + path_);
        file_ = nullptr;
        if (!s.ok()) return s;
    }
    row_count_ = 0;
    return Status::OK();
}

Status ColumnFile::Append(std::span<const int64_t> values) {
    if (type_ != ColumnType::TIMESTAMP && type_ != ColumnType::INT)
        return Status(StatusCode::INVALID_ARGUMENT, "column is not integer type");
    return AppendBytes(values.data(), values.size(), sizeof(int64_t));
}

Status ColumnFile::Append(std::span<const double> values) {
    if (type_ != ColumnType::FLOAT) return Status(StatusCode::INVALID_ARGUMENT, "column is not float type");
    return AppendBytes(values.data(), values.size(), sizeof(double));
}

Status ColumnFile::AppendBytes(const void* data, size_t count, size_t elem_size) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (count == 0) return Status::OK();

    size_t written = std::fwrite(data, elem_size, count, file_);
    if (written != count) return Status(StatusCode::IO_ERROR, "write failed: " + path_);

    row_count_ += count;
    return Status::OK();
}

Result<std::vector<int64_t>> ColumnFile::ReadAllInt64() {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (type_ != ColumnType::TIMESTAMP && type_ != ColumnType::INT)
        return Status(StatusCode::INVALID_ARGUMENT, "column is not integer type");

    std::vector<int64_t> out(row_count_);
    if (row_count_ == 0) return out;

    std::rewind(file_);  // 回到文件头
    size_t n = std::fread(out.data(), sizeof(int64_t), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::IO_ERROR, "read failed: " + path_);
    return out;
}

Result<std::vector<double>> ColumnFile::ReadAllFloat64() {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (type_ != ColumnType::FLOAT) return Status(StatusCode::INVALID_ARGUMENT, "column is not float type");

    std::vector<double> out(row_count_);
    if (row_count_ == 0) return out;

    std::rewind(file_);
    size_t n = std::fread(out.data(), sizeof(double), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::IO_ERROR, "read failed: " + path_);
    return out;
}

}  // namespace wavedb
