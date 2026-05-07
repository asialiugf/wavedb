// 列压缩/解压实现：NONE 透传 + DoD（Delta-of-Delta + zstd）。
//
// DoD 压缩流程（压缩）：
//   1. 将 raw 字节解释为 int64_t 数组
//   2. 计算一阶差分 delta[i] = t[i+1] - t[i]
//   3. 计算二阶差分 DoD[0]=delta[0], DoD[i>0]=delta[i]-delta[i-1]
//   4. first_delta=DoD[0] 原样存储；DoD[1..] 做 zigzag 编码后经 zstd 压缩
//   5. 输出：[base:8B][first_delta:8B][dod_count:2B][compressed_size:4B][zstd_data...]
//
// DoD 解压（解压）：
//   1. 读 header → 得到 base, first_delta, dod_count, compressed_size
//   2. zstd 解压 → zigzag-decode → 得到 DoD[1..]
//   3. 重建 delta[i] = delta[i-1] + DoD[i]
//   4. 重建 t[0]=base, t[i]=t[i-1]+delta[i-1]

#include "src/compression/compression.h"

#include <algorithm>
#include <cstring>
#include <zstd.h>

namespace wavedb {

// ── 工具函数：zigzag 编码/解码 ────────────────────────────────────────────

// zigzag 编码：将有符号 int64 映射到无符号 uint64，小绝对值 → 小无符号值
// 公式：(v << 1) ^ (v >> 63)
static inline uint64_t ZigzagEncode(int64_t v) {
    return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

// zigzag 解码：ZigzagEncode 的逆运算
// 公式：(v >> 1) ^ -(v & 1)
static inline int64_t ZigzagDecode(uint64_t v) {
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
}

// ── DoD 压缩头大小常量 ───────────────────────────────────────────────────
// base_value(8) + first_delta(8) + dod_count(2) + compressed_size(4) = 22 bytes
static constexpr size_t kDoDHeaderSize = 22;

// ── NONE 压缩（透传）─────────────────────────────────────────────────────

static std::vector<uint8_t> CompressNone(const uint8_t* raw, size_t raw_len) {
    std::vector<uint8_t> out(raw_len);
    if (raw_len > 0) std::memcpy(out.data(), raw, raw_len);
    return out;
}

static std::vector<uint8_t> DecompressNone(const uint8_t* data, size_t data_len, size_t output_len) {
    (void)output_len;
    std::vector<uint8_t> out(data_len);
    if (data_len > 0) std::memcpy(out.data(), data, data_len);
    return out;
}

// ── zstd 辅助 ────────────────────────────────────────────────────────────

// 用 zstd 压缩 src 数据，返回压缩后字节序列。
static std::vector<uint8_t> ZstdCompress(const void* src, size_t src_size) {
    size_t bound = ZSTD_compressBound(src_size);
    std::vector<uint8_t> out(bound);
    size_t n = ZSTD_compress(out.data(), out.size(), src, src_size, /*level=*/1);
    // ZSTD_compress 失败时返回错误码（0 也表示错误）
    if (ZSTD_isError(n)) {
        // 压缩失败时回退到透传
        out.resize(src_size);
        std::memcpy(out.data(), src, src_size);
        return out;
    }
    out.resize(n);
    return out;
}

// 用 zstd 解压 data，期望输出 output_size 字节。
static std::vector<uint8_t> ZstdDecompress(const void* data, size_t data_size, size_t output_size) {
    std::vector<uint8_t> out(output_size);
    size_t n = ZSTD_decompress(out.data(), out.size(), data, data_size);
    if (ZSTD_isError(n) || n != output_size) {
        // 解压失败时回退：尝试直接当做透传数据
        out.resize(data_size);
        if (data_size > 0) std::memcpy(out.data(), data, data_size);
        return out;
    }
    return out;
}

// ── DoD 压缩 ─────────────────────────────────────────────────────────────

// DoD 压缩：raw_len 字节的 int64_t 数组 → 压缩后字节序列。
// 布局: [base:8B][first_delta:8B][dod_count:2B][compressed_size:4B][zstd_data:...]
static std::vector<uint8_t> CompressDoD(const uint8_t* raw, size_t raw_len) {
    size_t row_count = raw_len / sizeof(int64_t);
    const auto* timestamps = reinterpret_cast<const int64_t*>(raw);

    std::vector<uint8_t> out;
    out.reserve(raw_len + 64);  // 预留空间，避免 insert 时 vector 无容量警告

    // 写入 base_value（t0）
    int64_t base = timestamps[0];
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&base),
               reinterpret_cast<const uint8_t*>(&base) + sizeof(base));

    // 写入 first_delta（t1 - t0 = DoD[0]）
    int64_t first_delta = (row_count >= 2) ? (timestamps[1] - timestamps[0]) : 0;
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&first_delta),
               reinterpret_cast<const uint8_t*>(&first_delta) + sizeof(first_delta));

    // 计算 DoD 个数（= row_count - 2）
    if (row_count <= 2) {
        uint16_t dod_count = 0;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&dod_count),
                   reinterpret_cast<const uint8_t*>(&dod_count) + sizeof(dod_count));
        uint32_t compressed_size = 0;
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(&compressed_size),
                   reinterpret_cast<const uint8_t*>(&compressed_size) + sizeof(compressed_size));
        return out;
    }

    // 计算 delta 和 DoD
    size_t delta_count = row_count - 1;
    std::vector<int64_t> deltas(delta_count);
    for (size_t i = 0; i < delta_count; ++i) {
        deltas[i] = timestamps[i + 1] - timestamps[i];
    }

    // DoD[0] = delta[0] = first_delta（已存储）
    // DoD[i] = delta[i] - delta[i-1]  for i >= 1
    size_t dod_count = delta_count - 1;  // = row_count - 2
    std::vector<int64_t> dods(dod_count);
    for (size_t i = 1; i < delta_count; ++i) {
        dods[i - 1] = deltas[i] - deltas[i - 1];
    }

    // zigzag 编码 DoD 值
    std::vector<uint64_t> zigzagged(dod_count);
    for (size_t i = 0; i < dod_count; ++i) {
        zigzagged[i] = ZigzagEncode(dods[i]);
    }

    // zstd 压缩 zigzag 编码后的 DoD 值
    std::vector<uint8_t> zstd_data = ZstdCompress(zigzagged.data(), zigzagged.size() * sizeof(uint64_t));

    // 写入 dod_count（2B）
    uint16_t dc = static_cast<uint16_t>(dod_count);
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&dc),
               reinterpret_cast<const uint8_t*>(&dc) + sizeof(dc));

    // 写入 compressed_size（4B）
    uint32_t cs = static_cast<uint32_t>(zstd_data.size());
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&cs),
               reinterpret_cast<const uint8_t*>(&cs) + sizeof(cs));

    // 写入 zstd 压缩数据
    out.insert(out.end(), zstd_data.begin(), zstd_data.end());

    return out;
}

// DoD 解压：从压缩数据恢复 raw_len 字节的 int64_t 数组。
static std::vector<uint8_t> DecompressDoD(const uint8_t* data, size_t data_len, size_t output_len) {
    size_t row_count = output_len / sizeof(int64_t);
    std::vector<uint8_t> out(output_len);
    auto* timestamps = reinterpret_cast<int64_t*>(out.data());

    if (row_count == 0) return out;
    if (data_len < kDoDHeaderSize) return out;  // 损坏数据，返回空

    // 读取 base_value（t0）
    int64_t base;
    std::memcpy(&base, data, sizeof(base));
    timestamps[0] = base;

    // 读取 first_delta
    int64_t first_delta;
    std::memcpy(&first_delta, data + 8, sizeof(first_delta));

    if (row_count == 1) return out;

    // 读取 dod_count（2B）
    uint16_t dod_count;
    std::memcpy(&dod_count, data + 16, sizeof(dod_count));

    // 读取 compressed_size（4B）
    uint32_t compressed_size;
    std::memcpy(&compressed_size, data + 18, sizeof(compressed_size));

    if (row_count == 2) {
        timestamps[1] = timestamps[0] + first_delta;
        return out;
    }

    // zstd 解压 DoD 数据
    auto zstd_decompressed = ZstdDecompress(
        data + kDoDHeaderSize, compressed_size,
        dod_count * sizeof(uint64_t));

    if (zstd_decompressed.size() != dod_count * sizeof(uint64_t)) {
        // 解压失败：返回 base 填充
        for (size_t i = 1; i < row_count; ++i) timestamps[i] = base;
        return out;
    }

    const auto* zigzagged = reinterpret_cast<const uint64_t*>(zstd_decompressed.data());

    // zigzag 解码 + 重建 DoD 值
    // DoD[0] = first_delta（已知）
    // DoD[1..dod_count] = zigzag-decode
    std::vector<int64_t> dods(dod_count + 1);
    dods[0] = first_delta;
    for (size_t i = 0; i < dod_count; ++i) {
        dods[i + 1] = ZigzagDecode(zigzagged[i]);
    }

    // 重建 delta
    // delta[0] = DoD[0] = first_delta
    // delta[i] = delta[i-1] + DoD[i]
    size_t delta_count = dod_count + 1;  // = row_count - 1
    std::vector<int64_t> deltas(delta_count);
    deltas[0] = first_delta;
    for (size_t i = 1; i < delta_count; ++i) {
        deltas[i] = deltas[i - 1] + dods[i];
    }

    // 重建时间戳
    // t[0] = base, t[i] = t[i-1] + delta[i-1]
    for (size_t i = 1; i < row_count; ++i) {
        timestamps[i] = timestamps[i - 1] + deltas[i - 1];
    }

    return out;
}

// ── ZSTD 压缩（FLOAT 列）───────────────────────────────────────────────
// 布局: [compressed_size:4B][zstd_data:...]

static constexpr size_t kZstdHeaderSize = 4;

static std::vector<uint8_t> CompressZstd(const uint8_t* raw, size_t raw_len) {
    auto zstd_data = ZstdCompress(raw, raw_len);

    std::vector<uint8_t> out;
    uint32_t cs = static_cast<uint32_t>(zstd_data.size());
    out.reserve(sizeof(cs) + zstd_data.size());
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(&cs),
               reinterpret_cast<const uint8_t*>(&cs) + sizeof(cs));
    out.insert(out.end(), zstd_data.begin(), zstd_data.end());
    return out;
}

static std::vector<uint8_t> DecompressZstd(const uint8_t* data, size_t data_len, size_t output_len) {
    if (data_len < kZstdHeaderSize) {
        std::vector<uint8_t> out(data_len);
        if (data_len > 0) std::memcpy(out.data(), data, data_len);
        return out;
    }

    uint32_t compressed_size;
    std::memcpy(&compressed_size, data, sizeof(compressed_size));

    return ZstdDecompress(data + kZstdHeaderSize, compressed_size, output_len);
}

// ── 公开 API ─────────────────────────────────────────────────────────────

std::vector<uint8_t> CompressBlock(const uint8_t* raw, size_t raw_len, CompressionType ctype) {
    switch (ctype) {
        case CompressionType::NONE:
            return CompressNone(raw, raw_len);
        case CompressionType::DoD:
            return CompressDoD(raw, raw_len);
        case CompressionType::ZSTD:
            return CompressZstd(raw, raw_len);
    }
    return {};
}

std::vector<uint8_t> DecompressBlock(const uint8_t* data, size_t data_len, size_t output_len, CompressionType ctype) {
    switch (ctype) {
        case CompressionType::NONE:
            return DecompressNone(data, data_len, output_len);
        case CompressionType::DoD:
            return DecompressDoD(data, data_len, output_len);
        case CompressionType::ZSTD:
            return DecompressZstd(data, data_len, output_len);
    }
    return {};
}

}  // namespace wavedb
