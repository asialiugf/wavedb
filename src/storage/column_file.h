// 列文件——单列持久化存储（v0.4+ 块式压缩格式）。
//
// 新格式：
//   FileHeader (16B) + BlockIndex (N×8B) + Block 0..N-1 data
//   每 block 最多 2048 行，独立压缩。
//   旧格式兼容：无 "WCDB" magic → 按裸二进制读取。
//
// 文件操作模式："a+b" = append + read, create if needed。

#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "src/compression/compression.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 列文件头（16 bytes）
#pragma pack(push, 1)
struct ColFileHeader {
    char magic[4] = {'W', 'C', 'D', 'B'};
    uint16_t version = 1;
    uint16_t block_size = 2048;   // 每块最大行数
    uint16_t block_count = 0;
    uint8_t  compression = 0;     // CompressionType: 0=none, 1=DoD
    uint8_t  elem_size = 8;       // sizeof(element)
    uint32_t row_count = 0;
};
#pragma pack(pop)

static constexpr size_t kColFileHeaderSize = 16;

class ColumnFile {
  public:
    ColumnFile() = default;
    ~ColumnFile();

    ColumnFile(const ColumnFile&) = delete;
    ColumnFile& operator=(const ColumnFile&) = delete;

    ColumnFile(ColumnFile&& other) noexcept
        : path_(std::move(other.path_)), type_(other.type_), file_(other.file_),
          row_count_(other.row_count_), has_header_(other.has_header_),
          header_(other.header_), block_offsets_(std::move(other.block_offsets_)) {
        other.file_ = nullptr;
        other.row_count_ = 0;
        other.has_header_ = false;
    }

    ColumnFile& operator=(ColumnFile&& other) noexcept {
        if (this != &other) {
            Close();
            path_ = std::move(other.path_);
            type_ = other.type_;
            file_ = other.file_;
            row_count_ = other.row_count_;
            has_header_ = other.has_header_;
            header_ = other.header_;
            block_offsets_ = std::move(other.block_offsets_);
            other.file_ = nullptr;
            other.row_count_ = 0;
            other.has_header_ = false;
        }
        return *this;
    }

    // 以 a+b 模式打开列文件。exclusive=true 时加 flock(LOCK_EX)。
    // 自动检测新旧格式。
    static Result<ColumnFile> Open(std::string path, ColumnType type, bool exclusive = false);

    // 追加原始数据（旧格式兼容路径）。类型须匹配。
    Status Append(std::span<const int64_t> values);
    Status Append(std::span<const double> values);

    // 读取全部已有数据（自动检测格式，新格式自动解压）。
    Result<std::vector<int64_t>> ReadAllInt64();
    Result<std::vector<double>> ReadAllFloat64();

    // 从指定行偏移读取 count 行。
    Result<std::vector<int64_t>> ReadRangeInt64(size_t start, size_t count);
    Result<std::vector<double>> ReadRangeFloat64(size_t start, size_t count);

    // 强制刷盘。
    Status Flush();

    size_t row_count() const { return row_count_; }
    ColumnType type() const { return type_; }
    bool has_header() const { return has_header_; }

    // 刷盘 + fclose。
    Status Close();

    // --- v0.4 块式写入（压缩路径）---

    // 以块式格式创建新文件，写入 FileHeader（不写数据）。
    static Result<ColumnFile> CreateBlocked(std::string path, ColumnType type,
                                            CompressionType comp, bool exclusive);

    // 追加一个压缩块到文件尾，更新 header + block index。
    Status AppendBlock(const std::vector<uint8_t>& compressed_data);

    // 追加一批已压缩的 block 到文件尾（批量写入，最后统一更新 header）。
    // blocks: 每个元素是一块压缩数据。
    Status AppendBlocks(const std::vector<std::vector<uint8_t>>& blocks);

    // 重写文件头 + block index（in-progress m_ 用）。
    Status RewriteHeader();

    // 读取指定 block 的原始（未解压）数据。
    Result<std::vector<uint8_t>> ReadBlockData(size_t block_idx);

    const ColFileHeader& header() const { return header_; }

  private:
    Status AppendBytes(const void* data, size_t count, size_t elem_size);

    // 初始化新文件的 header
    Status InitHeader(CompressionType comp);

    // 检测并解析已有文件的 header
    Status DetectHeader();

    std::string path_;
    ColumnType type_ = ColumnType::INT;
    FILE* file_ = nullptr;
    size_t row_count_ = 0;

    // v0.4 块式格式
    bool has_header_ = false;
    ColFileHeader header_;
    std::vector<uint64_t> block_offsets_;  // 每块的 data_offset
};

}  // namespace wavedb
