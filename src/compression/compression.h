// 列压缩/解压——v0.4+ 块式压缩。
// 支持 NONE（透传）、DoD（Delta-of-Delta，INT/TIMESTAMP）、ZSTD（FLOAT）。
//
// DoD 压缩算法（INT/TIMESTAMP）：
//   原始:    t0, t1, t2, ..., tn
//   delta:   t1-t0, t2-t1, ..., tn-t(n-1)
//   DoD:     delta[0], delta[1]-delta[0], ..., delta[n-1]-delta[n-2]
//   zigzag 编码后经 zstd 压缩
//
// Block 内布局:
//   DoD: [base_value:8B][first_delta:8B][dod_count:2B][compressed_size:4B][zstd_data:...]
//   ZSTD: [compressed_size:4B][zstd_data:...]
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace wavedb {

// 压缩类型
enum class CompressionType : uint8_t {
    NONE = 0,  // 无压缩（透传原始字节）
    DoD  = 1,  // Delta-of-Delta + zstd（INT / TIMESTAMP 列）
    ZSTD = 2,  // zstd 直接压缩原始字节（FLOAT 列）
};

// 压缩：输入 raw_data（block_size × elem_size 字节）→ 返回压缩后数据。
// ctype=NONE：原样返回。ctype=DoD：raw_data 视为 int64_t 数组做 DoD + zstd 编码。
// raw_len 必须 > 0 且为 8 的倍数（DoD 模式下）。
std::vector<uint8_t> CompressBlock(const uint8_t* raw, size_t raw_len, CompressionType ctype);

// 解压：输入压缩数据 → 返回原始数据。output_len 为期望输出字节数。
// ctype=NONE：原样返回。ctype=DoD：从压缩数据解出 output_len 字节的 int64_t 数组。
std::vector<uint8_t> DecompressBlock(const uint8_t* data, size_t data_len, size_t output_len, CompressionType ctype);

}  // namespace wavedb
