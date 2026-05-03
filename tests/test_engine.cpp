#include <gtest/gtest.h>

#include <cstdio>

#include "src/common/types.h"
#include "src/engine/connection.h"
#include "src/engine/wavedb.h"

using namespace wavedb;

class EngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/wavedb_engine_test_XXXXXX";
        tmpdir_ = ::mkdtemp(tmpl);
    }
    void TearDown() override { std::system(("rm -rf " + tmpdir_).c_str()); }
    std::string tmpdir_;
    static constexpr int64_t base_ts = 1767264600000000LL;
};

TEST_F(EngineTest, CreateTableAndQuery) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("ticks");
    schema.AddColumn("ts", ColumnType::kTimestamp, TimePrecision::SECOND);
    schema.AddColumn("price", ColumnType::kFloat);
    schema.AddColumn("volume", ColumnType::kInt);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("ticks");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, 100.0, int64_t(1000));
    app->AppendRow(base_ts + 60'000'000LL, 101.0, int64_t(2000));
    app->AppendRow(base_ts + 120'000'000LL, 102.0, int64_t(3000));
    ASSERT_TRUE(app->Close().ok());

    auto r = conn.Select("ticks");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
    EXPECT_EQ(r->ColumnCount(), 3u);
    EXPECT_EQ(std::get<int64_t>(r->rows[0][0]), base_ts);
    EXPECT_EQ(std::get<double>(r->rows[1][1]), 101.0);
    EXPECT_EQ(std::get<int64_t>(r->rows[2][2]), int64_t(3000));
}

TEST_F(EngineTest, MultiPartQuery) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("multi");
    schema.AddColumn("ts", ColumnType::kTimestamp);
    schema.AddColumn("val", ColumnType::kFloat);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    for (int p = 0; p < 3; ++p) {
        auto app = conn.CreateAppender("multi");
        ASSERT_TRUE(app.ok());
        for (int i = 0; i < 2; ++i) app->AppendRow(base_ts + (p * 2 + i) * 60'000'000LL, double(p * 10 + i));
        ASSERT_TRUE(app->Close().ok());
    }

    auto r = conn.Select("multi");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 6u);
}

TEST_F(EngineTest, TimeRangeFilter) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("range_test");
    schema.AddColumn("ts", ColumnType::kTimestamp);
    schema.AddColumn("val", ColumnType::kInt);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("range_test");
    ASSERT_TRUE(app.ok());
    for (int i = 0; i < 5; ++i) app->AppendRow(base_ts + i * 60'000'000LL, int64_t(i));
    ASSERT_TRUE(app->Close().ok());

    auto r = conn.Select("range_test", {"*"}, base_ts + 60'000'000LL, base_ts + 180'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
}

TEST_F(EngineTest, ReadOnly) {
    {
        auto db = WaveDB::Open(tmpdir_);
        ASSERT_TRUE(db.ok());
        Connection conn(*db);
        TableSchema schema("ro_test");
        schema.AddColumn("ts", ColumnType::kTimestamp);
        ASSERT_TRUE(conn.CreateTable(schema).ok());
    }
    {
        auto db = WaveDB::Open(tmpdir_, {.read_only = true});
        ASSERT_TRUE(db.ok());
        EXPECT_TRUE(db->read_only());
        Connection conn(*db);

        auto r = conn.Select("ro_test");
        ASSERT_TRUE(r.ok());

        auto s = conn.Insert("ro_test", {base_ts});
        EXPECT_FALSE(s.ok());
    }
}
