#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <vector>

#include "src/compression/compression.h"
#include "src/storage/column_file.h"
#include "src/storage/part.h"
#include "src/storage/part_manager.h"
#include "wavedb/schema.h"
#include "wavedb/types.h"

using namespace wavedb;

class StorageTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/wavedb_storage_test_XXXXXX";
        tmpdir_ = ::mkdtemp(tmpl);
    }
    void TearDown() override {
        {
            std::error_code ec;
            std::filesystem::remove_all(tmpdir_, ec);
        };
    }
    std::string tmpdir_;
};

TEST_F(StorageTest, ColumnFileAppendInt64) {
    std::string path = tmpdir_ + "/test.col";
    auto cf = ColumnFile::Open(path, ColumnType::INT);
    ASSERT_TRUE(cf.ok());
    EXPECT_EQ(cf->row_count(), 0u);

    std::vector<int64_t> data = {1, 2, 3, 4, 5};
    ASSERT_TRUE(cf->Append(data).ok());
    EXPECT_EQ(cf->row_count(), 5u);

    auto result = cf->ReadAllInt64();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->size(), 5u);
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ((*result)[i], int64_t(i + 1));

    ASSERT_TRUE(cf->Close().ok());
}

TEST_F(StorageTest, ColumnFileReopen) {
    std::string path = tmpdir_ + "/reopen.col";
    {
        auto cf = ColumnFile::Open(path, ColumnType::INT);
        ASSERT_TRUE(cf.ok());
        std::vector<int64_t> data = {10, 20, 30};
        ASSERT_TRUE(cf->Append(data).ok());
        ASSERT_TRUE(cf->Close().ok());
    }
    {
        auto cf = ColumnFile::Open(path, ColumnType::INT);
        ASSERT_TRUE(cf.ok());
        EXPECT_EQ(cf->row_count(), 3u);
        ASSERT_TRUE(cf->Close().ok());
    }
}

TEST_F(StorageTest, ColumnFileFloat) {
    std::string path = tmpdir_ + "/float.col";
    auto cf = ColumnFile::Open(path, ColumnType::FLOAT);
    ASSERT_TRUE(cf.ok());

    std::vector<double> data = {1.5, 2.5, 3.5};
    ASSERT_TRUE(cf->Append(data).ok());
    EXPECT_EQ(cf->row_count(), 3u);

    auto result = cf->ReadAllFloat64();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result->size(), 3u);

    ASSERT_TRUE(cf->Close().ok());
}

TEST_F(StorageTest, PartCreate) {
    TableSchema schema("test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::FLOAT);
    schema.AddColumn("cnt", ColumnType::INT);

    std::vector<std::vector<Value>> columns(3);
    columns[0].push_back(int64_t(1767264600000000LL));
    columns[0].push_back(int64_t(1767264660000000LL));
    columns[1].push_back(100.5);
    columns[1].push_back(101.0);
    columns[2].push_back(int64_t(10));
    columns[2].push_back(int64_t(20));

    int64_t min_ts = 1767264600000000LL;
    int64_t max_ts = 1767264660000000LL;
    std::string part_dir = tmpdir_ + "/part_test";

    auto part = Part::CreateWithPath(part_dir, schema, columns, min_ts, max_ts);
    ASSERT_TRUE(part.ok());
    EXPECT_EQ(part->row_count(), 2u);
    EXPECT_EQ(part->min_ts(), min_ts);
    EXPECT_EQ(part->max_ts(), max_ts);

    EXPECT_EQ(::access((part_dir + "/meta.json").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/ts.col").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/val.col").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/cnt.col").c_str(), F_OK), 0);
}

TEST_F(StorageTest, PartOpenAndRead) {
    TableSchema schema("test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::FLOAT);

    std::string part_dir = tmpdir_ + "/part_read";
    std::vector<std::vector<Value>> columns(2);
    columns[0].push_back(int64_t(100));
    columns[1].push_back(1.5);

    auto created = Part::CreateWithPath(part_dir, schema, columns, 100, 100);
    ASSERT_TRUE(created.ok());

    auto part = Part::Open(part_dir, schema);
    ASSERT_TRUE(part.ok());
    EXPECT_EQ(part->row_count(), 1u);

    auto ts_col = part->ReadColumn(0, ColumnType::TIMESTAMP);
    ASSERT_TRUE(ts_col.ok());
    EXPECT_EQ(std::get<int64_t>((*ts_col)[0]), int64_t(100));

    auto val_col = part->ReadColumn(1, ColumnType::FLOAT);
    ASSERT_TRUE(val_col.ok());
    EXPECT_EQ(std::get<double>((*val_col)[0]), 1.5);
}

TEST_F(StorageTest, PartManagerTimePruning) {
    TableSchema schema("pm");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::FLOAT);

    std::string table_dir = tmpdir_ + "/pm_table";
    ::mkdir(table_dir.c_str(), 0755);
    std::string parts_dir = table_dir + "/parts";
    ::mkdir(parts_dir.c_str(), 0755);

    // 3 Parts: [0,100], [200,300], [400,500]
    for (int p = 0; p < 3; ++p) {
        std::string dir = parts_dir + "/m_00000000_00000" + std::to_string(p + 1);
        int64_t ts = p * 200;
        std::vector<std::vector<Value>> cols(2);
        cols[0].push_back(int64_t(ts));
        cols[1].push_back(double(p));
        ASSERT_TRUE(Part::CreateWithPath(dir, schema, cols, ts, ts).ok());
    }

    auto pm = PartManager::Open(table_dir, schema);
    ASSERT_TRUE(pm.ok());
    EXPECT_EQ(pm->all_parts().size(), 3u);
    EXPECT_EQ(pm->total_rows(), 3u);

    // [150, 350) → 只有 Part 002
    auto parts = pm->GetPartsInRange(150, 350);
    EXPECT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0]->min_ts(), 200);

    // [0, 1000) → 全部
    auto all = pm->GetPartsInRange(0, 1000);
    EXPECT_EQ(all.size(), 3u);
}

// ── 压缩测试（DoD / NONE）────────────────────────────────────────────────

TEST_F(StorageTest, CompressNoneRoundTrip) {
    // NONE 压缩：往返后数据完全一致
    std::vector<int64_t> data = {1, 2, 3, 4, 5};
    size_t raw_len = data.size() * sizeof(int64_t);
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(data.data()), raw_len, CompressionType::NONE);
    ASSERT_EQ(compressed.size(), raw_len);
    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), raw_len, CompressionType::NONE);
    ASSERT_EQ(decompressed.size(), raw_len);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    for (size_t i = 0; i < data.size(); ++i) EXPECT_EQ(result[i], data[i]);
}

TEST_F(StorageTest, CompressDoDSingleRow) {
    // 单行：base_value 写入，first_delta=0，dod_count=0，compressed_size=0
    int64_t ts = 1767264600000000LL;
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(&ts), 8, CompressionType::DoD);
    // 22 字节 header（base 8 + first_delta 8 + dod_count 2 + compressed_size 4），无 dod_data
    EXPECT_EQ(compressed.size(), 22u);
    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), 8, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), 8u);
    EXPECT_EQ(*reinterpret_cast<const int64_t*>(decompressed.data()), ts);
}

TEST_F(StorageTest, CompressDoDTwoRows) {
    // 两行：base + first_delta, dod_count=0, compressed_size=0
    int64_t ts[2] = {1000000LL, 2000000LL};
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(ts), 16, CompressionType::DoD);
    EXPECT_EQ(compressed.size(), 22u);
    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), 16, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), 16u);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    EXPECT_EQ(result[0], 1000000LL);
    EXPECT_EQ(result[1], 2000000LL);
}

TEST_F(StorageTest, CompressDoDRegularIntervals) {
    // 等间隔时间戳：DoD 全为零 → bit_width=0，高度压缩
    std::vector<int64_t> timestamps;
    int64_t t0 = 1767264600000000LL;
    int64_t step = 60'000'000LL;  // 每分钟
    for (int i = 0; i < 1000; ++i) timestamps.push_back(t0 + i * step);

    size_t raw_len = timestamps.size() * sizeof(int64_t);
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(timestamps.data()), raw_len, CompressionType::DoD);

    // 等间隔 → DoD 全零 → zstd 高效压缩 → dod_data 极小
    EXPECT_GT(raw_len, compressed.size());  // 有压缩效果
    EXPECT_LE(compressed.size(), 50u);  // 1000 行等间隔 ≤ ~50B

    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), raw_len, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), raw_len);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    for (size_t i = 0; i < timestamps.size(); ++i) EXPECT_EQ(result[i], timestamps[i]);
}

TEST_F(StorageTest, CompressDoDIrregularIntervals) {
    // 不规则间隔：DoD 非零 → bit_width > 0 → 压缩但体积略大
    std::vector<int64_t> timestamps;
    int64_t t0 = 1767264600000000LL;
    // 不规则步长
    int64_t steps[] = {1'000'000LL, 5'000'000LL, 2'000'000LL, 8'000'000LL, 3'000'000LL};
    timestamps.push_back(t0);
    for (size_t i = 0; i < 5; ++i) timestamps.push_back(timestamps.back() + steps[i]);

    size_t raw_len = timestamps.size() * sizeof(int64_t);
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(timestamps.data()), raw_len, CompressionType::DoD);

    // 往返验证
    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), raw_len, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), raw_len);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    for (size_t i = 0; i < timestamps.size(); ++i) EXPECT_EQ(result[i], timestamps[i]);
}

TEST_F(StorageTest, CompressDoDNegativeDoD) {
    // 递减时间戳（虽然不现实，但算法应正确处理负 DoD）
    std::vector<int64_t> timestamps;
    int64_t t0 = 1767264600000000LL;
    for (int i = 0; i < 5; ++i) timestamps.push_back(t0 - i * 10'000'000LL);

    size_t raw_len = timestamps.size() * sizeof(int64_t);
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(timestamps.data()), raw_len, CompressionType::DoD);
    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), raw_len, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), raw_len);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    for (size_t i = 0; i < timestamps.size(); ++i) EXPECT_EQ(result[i], timestamps[i]);
}

// ── Part 块式压缩测试 ──────────────────────────────────────────────────

static TableSchema MakeTestSchema() {
    TableSchema schema("test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::FLOAT);
    schema.AddColumn("cnt", ColumnType::INT);
    return schema;
}

static std::vector<std::vector<Value>> MakeColumns(size_t nrows, int64_t t0) {
    std::vector<std::vector<Value>> cols(3);
    for (size_t r = 0; r < nrows; ++r) {
        cols[0].push_back(t0 + static_cast<int64_t>(r) * 60'000'000LL);
        cols[1].push_back(100.0 + static_cast<double>(r));
        cols[2].push_back(static_cast<int64_t>(r));
    }
    return cols;
}

TEST_F(StorageTest, PartCreateBlockedAndRead) {
    // CreateBlocked 写压缩 Part → Open 读取 → 数据一致
    TableSchema schema = MakeTestSchema();
    std::string part_dir = tmpdir_ + "/m_part_blocked";
    size_t nrows = 3000;
    auto cols = MakeColumns(nrows, 1767264600000000LL);

    auto part = Part::CreateBlocked(part_dir, schema, cols, 1767264600000000LL,
                                     1767264600000000LL + int64_t(nrows - 1) * 60'000'000LL);
    ASSERT_TRUE(part.ok());
    EXPECT_EQ(part->row_count(), nrows);

    // 验证列文件存在
    EXPECT_EQ(::access((part_dir + "/ts.col").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/val.col").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/cnt.col").c_str(), F_OK), 0);
    EXPECT_EQ(::access((part_dir + "/meta.json").c_str(), F_OK), 0);

    // 重新打开读取
    auto opened = Part::Open(part_dir, schema);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened->row_count(), nrows);

    // 读 TS 列验证
    auto ts_col = opened->ReadColumn(0, ColumnType::TIMESTAMP);
    ASSERT_TRUE(ts_col.ok());
    ASSERT_EQ(ts_col->size(), nrows);
    for (size_t i = 0; i < nrows; ++i)
        EXPECT_EQ(std::get<int64_t>((*ts_col)[i]), 1767264600000000LL + int64_t(i) * 60'000'000LL);

    // 读 FLOAT 列验证
    auto val_col = opened->ReadColumn(1, ColumnType::FLOAT);
    ASSERT_TRUE(val_col.ok());
    for (size_t i = 0; i < nrows; ++i)
        EXPECT_DOUBLE_EQ(std::get<double>((*val_col)[i]), 100.0 + double(i));

    // 读 INT 列验证
    auto cnt_col = opened->ReadColumn(2, ColumnType::INT);
    ASSERT_TRUE(cnt_col.ok());
    for (size_t i = 0; i < nrows; ++i)
        EXPECT_EQ(std::get<int64_t>((*cnt_col)[i]), int64_t(i));
}

TEST_F(StorageTest, PartCreateBlockedReadRange) {
    // 范围读取压缩 Part
    TableSchema schema = MakeTestSchema();
    std::string part_dir = tmpdir_ + "/m_part_range";
    size_t nrows = 2048;
    auto cols = MakeColumns(nrows, 1767264600000000LL);

    auto part = Part::CreateBlocked(part_dir, schema, cols,
                                     1767264600000000LL,
                                     1767264600000000LL + int64_t(nrows - 1) * 60'000'000LL);
    ASSERT_TRUE(part.ok());

    auto opened = Part::Open(part_dir, schema);
    ASSERT_TRUE(opened.ok());

    // 读 TS 列 [100, 200)
    auto range = opened->ReadColumnRange(0, ColumnType::TIMESTAMP, 100, 100);
    ASSERT_TRUE(range.ok());
    EXPECT_EQ(range->size(), 100u);
    EXPECT_EQ(std::get<int64_t>((*range)[0]), 1767264600000000LL + 100 * 60'000'000LL);
    EXPECT_EQ(std::get<int64_t>((*range)[99]), 1767264600000000LL + 199 * 60'000'000LL);
}

TEST_F(StorageTest, PartCreateBlockedProgressiveAppend) {
    // 渐进式追加：CreateBlocked 初始 → AppendColumnsBlocked → 读取
    TableSchema schema = MakeTestSchema();
    std::string part_dir = tmpdir_ + "/m_part_prog";

    // 第一批：500 行
    size_t batch1 = 500;
    auto cols1 = MakeColumns(batch1, 1767264600000000LL);
    auto part = Part::CreateBlocked(part_dir, schema, cols1,
                                     1767264600000000LL,
                                     1767264600000000LL + int64_t(batch1 - 1) * 60'000'000LL);
    ASSERT_TRUE(part.ok());
    EXPECT_EQ(part->row_count(), batch1);

    // 第二批：追加 800 行
    size_t batch2 = 800;
    int64_t t1 = 1767264600000000LL + int64_t(batch1) * 60'000'000LL;
    auto cols2 = MakeColumns(batch2, t1);
    // 需要重新 Open 因为 CreateBlocked 返回的 Part 没有持有文件句柄
    auto opened = Part::Open(part_dir, schema);
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened->AppendColumnsBlocked(cols2).ok());

    // 验证总行数
    auto final = Part::Open(part_dir, schema);
    ASSERT_TRUE(final.ok());
    EXPECT_EQ(final->row_count(), batch1 + batch2);

    // 验证连续性
    auto ts_col = final->ReadColumn(0, ColumnType::TIMESTAMP);
    ASSERT_TRUE(ts_col.ok());
    ASSERT_EQ(ts_col->size(), batch1 + batch2);
    for (size_t i = 0; i < batch1 + batch2; ++i)
        EXPECT_EQ(std::get<int64_t>((*ts_col)[i]), 1767264600000000LL + int64_t(i) * 60'000'000LL);
}

TEST_F(StorageTest, PartCreateBlockedSmallBatch) {
    // 小于 block_size 的批次
    TableSchema schema = MakeTestSchema();
    std::string part_dir = tmpdir_ + "/m_part_small";
    size_t nrows = 50;

    int64_t t0 = 1767264600000000LL;
    std::vector<std::vector<Value>> cols(3);
    for (size_t r = 0; r < nrows; ++r) {
        cols[0].push_back(t0 + static_cast<int64_t>(r) * 1'000'000LL);
        cols[1].push_back(r * 1.5);
        cols[2].push_back(static_cast<int64_t>(r * 10));
    }

    auto part = Part::CreateBlocked(part_dir, schema, cols, t0,
                                     t0 + int64_t(nrows - 1) * 1'000'000LL);
    ASSERT_TRUE(part.ok());
    EXPECT_EQ(part->row_count(), nrows);

    auto opened = Part::Open(part_dir, schema);
    ASSERT_TRUE(opened.ok());
    auto ts_col = opened->ReadColumn(0, ColumnType::TIMESTAMP);
    ASSERT_TRUE(ts_col.ok());
    EXPECT_EQ(ts_col->size(), nrows);
    for (size_t i = 0; i < nrows; ++i)
        EXPECT_EQ(std::get<int64_t>((*ts_col)[i]), t0 + int64_t(i) * 1'000'000LL);
}

TEST_F(StorageTest, CompressDoDLargeBlock) {
    // 大块 DoD 压缩：4096 行不规则步长 → 往返正确
    std::vector<int64_t> timestamps;
    int64_t t0 = 1767264600000000LL;
    timestamps.push_back(t0);
    for (int i = 1; i < 4096; ++i) {
        int64_t step = 60'000'000LL + (i % 10) * 1'000'000LL;  // 略有抖动
        timestamps.push_back(timestamps.back() + step);
    }

    size_t raw_len = timestamps.size() * sizeof(int64_t);
    auto compressed = CompressBlock(reinterpret_cast<const uint8_t*>(timestamps.data()), raw_len, CompressionType::DoD);
    // 验证有压缩效果
    EXPECT_LT(compressed.size(), raw_len);

    auto decompressed = DecompressBlock(compressed.data(), compressed.size(), raw_len, CompressionType::DoD);
    ASSERT_EQ(decompressed.size(), raw_len);
    auto* result = reinterpret_cast<const int64_t*>(decompressed.data());
    for (size_t i = 0; i < timestamps.size(); ++i) EXPECT_EQ(result[i], timestamps[i]);
}
