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
    FILE* f = std::fopen(path.c_str(), "a+b");  // append+read, create if needed
    if (!f) return Status(StatusCode::kIOError, "cannot open: " + path);

    // 从文件大小反算已有行数
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
    if (file_ && std::fflush(file_) != 0) return Status(StatusCode::kIOError, "flush failed: " + path_);
    return Status::OK();
}

Status ColumnFile::Close() {
    if (file_) {
        Status s = Flush();
        if (std::fclose(file_) != 0) return Status(StatusCode::kIOError, "close failed: " + path_);
        file_ = nullptr;
        if (!s.ok()) return s;
    }
    row_count_ = 0;
    return Status::OK();
}

Status ColumnFile::Append(std::span<const int64_t> values) {
    if (type_ != ColumnType::kTimestamp && type_ != ColumnType::kInt)
        return Status(StatusCode::kInvalidArgument, "column is not integer type");
    return AppendBytes(values.data(), values.size(), sizeof(int64_t));
}

Status ColumnFile::Append(std::span<const double> values) {
    if (type_ != ColumnType::kFloat) return Status(StatusCode::kInvalidArgument, "column is not float type");
    return AppendBytes(values.data(), values.size(), sizeof(double));
}

Status ColumnFile::AppendBytes(const void* data, size_t count, size_t elem_size) {
    if (!file_) return Status(StatusCode::kInternal, "file not open");
    if (count == 0) return Status::OK();

    size_t written = std::fwrite(data, elem_size, count, file_);
    if (written != count) return Status(StatusCode::kIOError, "write failed: " + path_);

    row_count_ += count;
    return Status::OK();
}

Result<std::vector<int64_t>> ColumnFile::ReadAllInt64() {
    if (!file_) return Status(StatusCode::kInternal, "file not open");
    if (type_ != ColumnType::kTimestamp && type_ != ColumnType::kInt)
        return Status(StatusCode::kInvalidArgument, "column is not integer type");

    std::vector<int64_t> out(row_count_);
    if (row_count_ == 0) return out;

    std::rewind(file_);
    size_t n = std::fread(out.data(), sizeof(int64_t), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::kIOError, "read failed: " + path_);
    return out;
}

Result<std::vector<double>> ColumnFile::ReadAllFloat64() {
    if (!file_) return Status(StatusCode::kInternal, "file not open");
    if (type_ != ColumnType::kFloat) return Status(StatusCode::kInvalidArgument, "column is not float type");

    std::vector<double> out(row_count_);
    if (row_count_ == 0) return out;

    std::rewind(file_);
    size_t n = std::fread(out.data(), sizeof(double), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::kIOError, "read failed: " + path_);
    return out;
}

}  // namespace wavedb
