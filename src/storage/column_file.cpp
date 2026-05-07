// ColumnFile 实现：定长二进制列文件 + v0.4 块式压缩格式。

#include "src/storage/column_file.h"

#include <sys/file.h>
#include <sys/stat.h>

#include <cstring>

namespace wavedb {

ColumnFile::~ColumnFile() {
    if (file_) std::fclose(file_);
}

// ---- Open / Format Detection ----

Result<ColumnFile> ColumnFile::Open(std::string path, ColumnType type, bool exclusive) {
    FILE* f = std::fopen(path.c_str(), "a+b");
    if (!f) return Status(StatusCode::IO_ERROR, "cannot open: " + path);

    if (exclusive) {
        if (::flock(::fileno(f), LOCK_EX) != 0) {
            std::fclose(f);
            return Status(StatusCode::INTERNAL, "cannot lock: " + path);
        }
    }

    ColumnFile cf;
    cf.path_ = std::move(path);
    cf.type_ = type;
    cf.file_ = f;

    // 检测格式：读前 4 字节判断是否为 "WCDB" 头
    struct stat st;
    if (::stat(cf.path_.c_str(), &st) == 0 && st.st_size >= 4) {
        char magic[4];
        std::rewind(f);
        if (std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "WCDB", 4) == 0) {
            cf.has_header_ = true;
            std::rewind(f);
            if (std::fread(&cf.header_, sizeof(ColFileHeader), 1, f) != 1) {
                std::fclose(f);
                return Status(StatusCode::PARSE_ERROR, "cannot read header: " + cf.path_);
            }
            cf.row_count_ = cf.header_.row_count;
            // 读 block offsets
            cf.block_offsets_.resize(cf.header_.block_count);
            if (cf.header_.block_count > 0) {
                if (std::fread(cf.block_offsets_.data(), sizeof(uint64_t), cf.header_.block_count, f) !=
                    static_cast<size_t>(cf.header_.block_count)) {
                    std::fclose(f);
                    return Status(StatusCode::PARSE_ERROR, "cannot read block index: " + cf.path_);
                }
            }
            return cf;
        }
    }

    // 旧格式或无文件：按 row_count = file_size / elem_size
    cf.has_header_ = false;
    size_t elem = ColumnTypeSize(type);
    cf.row_count_ = (st.st_size > 0) ? static_cast<size_t>(st.st_size) / elem : 0;
    return cf;
}

// ---- Create Blocked ----

Result<ColumnFile> ColumnFile::CreateBlocked(std::string path, ColumnType type,
                                             CompressionType comp, bool exclusive) {
    FILE* f = std::fopen(path.c_str(), "w+b");
    if (!f) return Status(StatusCode::IO_ERROR, "cannot create: " + path);

    if (exclusive) {
        if (::flock(::fileno(f), LOCK_EX) != 0) {
            std::fclose(f);
            return Status(StatusCode::INTERNAL, "cannot lock: " + path);
        }
    }

    ColumnFile cf;
    cf.path_ = std::move(path);
    cf.type_ = type;
    cf.file_ = f;
    cf.has_header_ = true;
    cf.header_ = {};
    cf.header_.elem_size = static_cast<uint8_t>(ColumnTypeSize(type));
    cf.header_.compression = static_cast<uint8_t>(comp);

    // 写空 header（后续 RewriteHeader 更新）
    std::rewind(f);
    if (std::fwrite(&cf.header_, sizeof(ColFileHeader), 1, f) != 1) {
        std::fclose(f);
        return Status(StatusCode::IO_ERROR, "cannot write header: " + cf.path_);
    }
    std::fflush(f);
    return cf;
}

// ---- Block Write ----

Status ColumnFile::AppendBlock(const std::vector<uint8_t>& data) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (!has_header_) return Status(StatusCode::INTERNAL, "not a blocked file");

    std::fseek(file_, 0, SEEK_END);
    uint64_t offset = static_cast<uint64_t>(std::ftell(file_));

    if (std::fwrite(data.data(), 1, data.size(), file_) != data.size())
        return Status(StatusCode::IO_ERROR, "write block failed: " + path_);

    block_offsets_.push_back(offset);
    header_.block_count = static_cast<uint16_t>(block_offsets_.size());
    return RewriteHeader();
}

Status ColumnFile::AppendBlocks(const std::vector<std::vector<uint8_t>>& blocks) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (!has_header_) return Status(StatusCode::INTERNAL, "not a blocked file");

    for (auto& blk : blocks) {
        std::fseek(file_, 0, SEEK_END);
        uint64_t offset = static_cast<uint64_t>(std::ftell(file_));
        if (std::fwrite(blk.data(), 1, blk.size(), file_) != blk.size())
            return Status(StatusCode::IO_ERROR, "write block failed: " + path_);
        block_offsets_.push_back(offset);
    }
    header_.block_count = static_cast<uint16_t>(block_offsets_.size());
    return RewriteHeader();
}

Status ColumnFile::RewriteHeader() {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    std::rewind(file_);
    if (std::fwrite(&header_, sizeof(ColFileHeader), 1, file_) != 1)
        return Status(StatusCode::IO_ERROR, "write header failed: " + path_);
    // 写 block offsets
    if (!block_offsets_.empty()) {
        if (std::fwrite(block_offsets_.data(), sizeof(uint64_t), block_offsets_.size(), file_) !=
            block_offsets_.size())
            return Status(StatusCode::IO_ERROR, "write block index failed: " + path_);
    }
    std::fflush(file_);
    return Status::OK();
}

// ---- Block Read ----

Result<std::vector<uint8_t>> ColumnFile::ReadBlockData(size_t block_idx) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (block_idx >= block_offsets_.size())
        return Status(StatusCode::INVALID_ARGUMENT, "block index out of range");

    uint64_t start = block_offsets_[block_idx];
    uint64_t end = (block_idx + 1 < block_offsets_.size()) ? block_offsets_[block_idx + 1]
                   : static_cast<uint64_t>(header_.row_count) * header_.elem_size;  // won't work for compressed
    // 正确的做法：如果最后一块，读 header_offset + header_size + index_size 到 EOF

    if (std::fseek(file_, static_cast<long>(start), SEEK_SET) != 0)
        return Status(StatusCode::IO_ERROR, "seek failed: " + path_);

    // 估算最大值
    size_t max_blk = header_.block_size * header_.elem_size;
    std::vector<uint8_t> buf(max_blk);
    size_t n = std::fread(buf.data(), 1, max_blk, file_);
    buf.resize(n);
    return buf;
}

// ---- Decompress helpers ----

static std::vector<uint8_t> ReadBlockAsRaw(ColumnFile& cf, size_t block_idx,
                                            size_t rows_in_block, size_t elem_size) {
    auto data = cf.ReadBlockData(block_idx);
    if (!data.ok()) return {};
    CompressionType ctype = static_cast<CompressionType>(cf.header().compression);
    return DecompressBlock(data->data(), data->size(), rows_in_block * elem_size, ctype);
}

template <typename T>
static Result<std::vector<T>> ReadAllBlocked(ColumnFile& cf) {
    size_t row_count = cf.row_count();
    if (row_count == 0) return std::vector<T>{};
    size_t elem_size = sizeof(T);
    size_t block_size = cf.header().block_size;

    std::vector<T> out;
    out.reserve(row_count);
    for (uint16_t bi = 0; bi < cf.header().block_count; ++bi) {
        size_t rows_in_block = block_size;
        if (bi == cf.header().block_count - 1)
            rows_in_block = row_count - bi * block_size;
        auto raw = ReadBlockAsRaw(cf, bi, rows_in_block, elem_size);
        const T* ptr = reinterpret_cast<const T*>(raw.data());
        for (size_t i = 0; i < rows_in_block; ++i) out.push_back(ptr[i]);
    }
    return out;
}

template <typename T>
static Result<std::vector<T>> ReadRangeBlocked(ColumnFile& cf, size_t start, size_t count) {
    if (count == 0) return std::vector<T>{};
    size_t block_size = cf.header().block_size;
    size_t first_block = start / block_size;
    size_t last_block = (start + count - 1) / block_size;

    std::vector<T> out;
    out.reserve(count);
    for (size_t bi = first_block; bi <= last_block && bi < cf.header().block_count; ++bi) {
        size_t rows_in_block = block_size;
        if (bi == cf.header().block_count - 1)
            rows_in_block = cf.row_count() - bi * block_size;
        auto raw = ReadBlockAsRaw(cf, bi, rows_in_block, sizeof(T));
        const T* ptr = reinterpret_cast<const T*>(raw.data());

        size_t block_start = bi * block_size;
        size_t from = (start > block_start) ? start - block_start : 0;
        size_t to = std::min(rows_in_block, start + count - block_start);
        for (size_t i = from; i < to; ++i) out.push_back(ptr[i]);
    }
    return out;
}

// ---- Legacy Read/Write ----

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
    if (row_count_ == 0) return std::vector<int64_t>{};
    if (has_header_) return ReadAllBlocked<int64_t>(*this);

    std::vector<int64_t> out(row_count_);
    std::rewind(file_);
    size_t n = std::fread(out.data(), sizeof(int64_t), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::IO_ERROR, "read failed: " + path_);
    return out;
}

Result<std::vector<double>> ColumnFile::ReadAllFloat64() {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (type_ != ColumnType::FLOAT) return Status(StatusCode::INVALID_ARGUMENT, "column is not float type");
    if (row_count_ == 0) return std::vector<double>{};
    if (has_header_) return ReadAllBlocked<double>(*this);

    std::vector<double> out(row_count_);
    std::rewind(file_);
    size_t n = std::fread(out.data(), sizeof(double), row_count_, file_);
    if (n != row_count_) return Status(StatusCode::IO_ERROR, "read failed: " + path_);
    return out;
}

Result<std::vector<int64_t>> ColumnFile::ReadRangeInt64(size_t start, size_t count) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (type_ != ColumnType::TIMESTAMP && type_ != ColumnType::INT)
        return Status(StatusCode::INVALID_ARGUMENT, "column is not integer type");
    if (start + count > row_count_)
        return Status(StatusCode::INVALID_ARGUMENT, "range exceeds row count");
    if (count == 0) return std::vector<int64_t>{};
    if (has_header_) return ReadRangeBlocked<int64_t>(*this, start, count);

    std::vector<int64_t> out(count);
    std::fseek(file_, static_cast<long>(start * sizeof(int64_t)), SEEK_SET);
    size_t n = std::fread(out.data(), sizeof(int64_t), count, file_);
    if (n != count) return Status(StatusCode::IO_ERROR, "range read failed: " + path_);
    return out;
}

Result<std::vector<double>> ColumnFile::ReadRangeFloat64(size_t start, size_t count) {
    if (!file_) return Status(StatusCode::INTERNAL, "file not open");
    if (type_ != ColumnType::FLOAT) return Status(StatusCode::INVALID_ARGUMENT, "column is not float type");
    if (start + count > row_count_)
        return Status(StatusCode::INVALID_ARGUMENT, "range exceeds row count");
    if (count == 0) return std::vector<double>{};
    if (has_header_) return ReadRangeBlocked<double>(*this, start, count);

    std::vector<double> out(count);
    std::fseek(file_, static_cast<long>(start * sizeof(double)), SEEK_SET);
    size_t n = std::fread(out.data(), sizeof(double), count, file_);
    if (n != count) return Status(StatusCode::IO_ERROR, "range read failed: " + path_);
    return out;
}

}  // namespace wavedb
