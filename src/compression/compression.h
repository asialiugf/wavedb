// 列压缩/解压——v0.4 占位，当前为无压缩透传。
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace wavedb {

// 压缩类型
enum class CompressionType : uint8_t {
    NONE = 0,  // 无压缩
    DoD  = 1,  // Delta-of-Delta（后续实现，仅 TIMESTAMP 列）
};

// 压缩：输入 raw_data（block_size × elem_size）→ 返回压缩后数据。
// v0.4 占位：原样返回。
inline std::vector<uint8_t> CompressBlock(const uint8_t* raw, size_t raw_len, CompressionType ctype) {
    (void)ctype;
    std::vector<uint8_t> out(raw_len);
    if (raw_len > 0) std::memcpy(out.data(), raw, raw_len);
    return out;
}

// 解压：输入压缩数据 → 返回原始数据。output_len 为期望输出长度。
// v0.4 占位：原样返回。
inline std::vector<uint8_t> DecompressBlock(const uint8_t* data, size_t data_len, size_t output_len, CompressionType ctype) {
    (void)ctype;
    std::vector<uint8_t> out(output_len);
    if (data_len > 0) std::memcpy(out.data(), data, data_len);
    return out;
}

}  // namespace wavedb
