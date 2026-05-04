#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdio>
#include <vector>

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
            int rc = std::system(("rm -rf " + tmpdir_).c_str());
            (void)rc;
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

    auto part = Part::Create(part_dir, schema, columns, min_ts, max_ts);
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

    auto created = Part::Create(part_dir, schema, columns, 100, 100);
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
        std::string dir = parts_dir + "/00" + std::to_string(p + 1);
        int64_t ts = p * 200;
        std::vector<std::vector<Value>> cols(2);
        cols[0].push_back(int64_t(ts));
        cols[1].push_back(double(p));
        ASSERT_TRUE(Part::Create(dir, schema, cols, ts, ts).ok());
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
