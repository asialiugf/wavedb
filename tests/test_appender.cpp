// Appender 测试：批量写入、缓冲、刷盘、数据校验。

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>

#include <gtest/gtest.h>

#include "src/storage/part_manager.h"
#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

using namespace wavedb;

static constexpr int64_t t0 = 1767264600000000LL;

static std::string MakeTempDir(const char* suffix) {
    std::string path = "/tmp/wavedb_app_test_";
    path += suffix;
    path += "_XXXXXX";
    char* tmpl = ::strdup(path.c_str());
    char* dir = ::mkdtemp(tmpl);
    std::string result(dir);
    ::free(tmpl);
    return result;
}

static void RemoveDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static size_t ForceMerge(Connection& conn, const std::string& table_name) {
    auto* schema = conn.GetTableSchema(table_name);
    std::string table_dir = conn.db().path() + "/" + table_name;
    auto pm = PartManager::Open(table_dir, *schema);
    if (!pm.ok()) return 0;
    return pm->MergeParts(schema->mergeConfig());
}

class AppenderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = MakeTempDir("app");
        auto db = WaveDB::Open(dir_);
        ASSERT_TRUE(db.ok());
        Connection conn(*db);
        ASSERT_TRUE(conn.Query("CREATE TABLE t (ts TIMESTAMP(SECOND), price FLOAT, vol INT)").ok());
    }
    void TearDown() override { RemoveDir(dir_); }
    std::string dir_;
};

TEST_F(AppenderTest, SingleAppend) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    ASSERT_TRUE(app->AppendRow(t0, 100.5, int64_t(500)).ok());
    EXPECT_EQ(app->buffered_rows(), 1u);
    EXPECT_EQ(app->total_rows(), 1u);
    ASSERT_TRUE(app->Close().ok());
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 1u);
    EXPECT_EQ(int64_t(r->Row(0)["ts"]), t0);
    EXPECT_EQ(double(r->Row(0)["price"]), 100.5);
    EXPECT_EQ(int64_t(r->Row(0)["vol"]), int64_t(500));
}

TEST_F(AppenderTest, VariadicAppend) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    ASSERT_TRUE(app->AppendRow(t0, 10.0, int64_t(100)).ok());
    ASSERT_TRUE(app->AppendRow(t0 + 60'000'000LL, 10.1, int64_t(110)).ok());
    ASSERT_TRUE(app->Flush().ok());
    ASSERT_TRUE(app->AppendRow(t0 + 120'000'000LL, 10.2, int64_t(120)).ok());
    ASSERT_TRUE(app->Close().ok());
    EXPECT_EQ(app->total_rows(), 3u);
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
    EXPECT_EQ(int64_t(r->Row(0)["ts"]), t0);
    EXPECT_EQ(double(r->Row(1)["price"]), 10.1);
    EXPECT_EQ(int64_t(r->Row(2)["vol"]), int64_t(120));
}

TEST_F(AppenderTest, MultipleBatches) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    for (int batch = 0; batch < 3; ++batch) {
        auto app = conn.CreateAppender("t");
        ASSERT_TRUE(app.ok());
        for (int i = 0; i < 2; ++i) {
            int64_t ts = t0 + (batch * 2 + i) * 60'000'000LL;
            ASSERT_TRUE(app->AppendRow(ts, double(100 + batch * 10 + i), int64_t(1000 + batch * 100 + i)).ok());
        }
        ASSERT_TRUE(app->Close().ok());
        EXPECT_EQ(app->total_rows(), 2u);
    }
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 6u);
}

TEST_F(AppenderTest, ColumnCountMismatch) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    EXPECT_FALSE(app->AppendRow(t0, 1.0).ok());
    EXPECT_FALSE(app->AppendRow(t0, 1.0, int64_t(1), int64_t(42)).ok());
    ASSERT_TRUE(app->AppendRow(t0, 1.0, int64_t(1)).ok());
    ASSERT_TRUE(app->Close().ok());
}

TEST_F(AppenderTest, TypeMismatch) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    EXPECT_FALSE(app->AppendRow(t0, int64_t(1), int64_t(1)).ok());
}

TEST_F(AppenderTest, EmptyFlush) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    ASSERT_TRUE(app->Flush().ok());
    ASSERT_TRUE(app->AppendRow(t0, 1.0, int64_t(1)).ok());
    ASSERT_TRUE(app->Close().ok());
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 1u);
}

TEST_F(AppenderTest, CounterState) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    EXPECT_EQ(app->buffered_rows(), 0u);
    EXPECT_EQ(app->total_rows(), 0u);
    ASSERT_TRUE(app->AppendRow(t0, 1.0, int64_t(1)).ok());
    EXPECT_EQ(app->buffered_rows(), 1u);
    EXPECT_EQ(app->total_rows(), 1u);
    ASSERT_TRUE(app->Flush().ok());
    EXPECT_EQ(app->buffered_rows(), 0u);
    EXPECT_EQ(app->total_rows(), 1u);
    ASSERT_TRUE(app->AppendRow(t0 + 1, 2.0, int64_t(2)).ok());
    ASSERT_TRUE(app->AppendRow(t0 + 2, 3.0, int64_t(3)).ok());
    EXPECT_EQ(app->buffered_rows(), 2u);
    EXPECT_EQ(app->total_rows(), 3u);
    ASSERT_TRUE(app->Close().ok());
}

TEST_F(AppenderTest, DtorAutoFlush) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    {
        auto app = conn.CreateAppender("t");
        ASSERT_TRUE(app.ok());
        ASSERT_TRUE(app->AppendRow(t0, 5.5, int64_t(99)).ok());
    }
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 1u);
    EXPECT_EQ(double(r->Row(0)["price"]), 5.5);
}

TEST_F(AppenderTest, ComputedColumns) {
    {
        auto db = WaveDB::Open(dir_);
        Connection conn(*db);
        ASSERT_TRUE(conn.AddColumn("t", "ma5", ColumnType::FLOAT).ok());
        ASSERT_TRUE(conn.AddColumn("t", "ma10", ColumnType::FLOAT).ok());
    }
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto app = conn.CreateAppender("t");
    ASSERT_TRUE(app.ok());
    std::deque<double> closes;
    for (int i = 0; i < 5; ++i) {
        double price = 100.0 + i;
        closes.push_back(price);
        double ma5 = 0.0, ma10 = 0.0;
        if (closes.size() >= 5) {
            double sum = 0;
            for (size_t j = closes.size() - 5; j < closes.size(); ++j) sum += closes[j];
            ma5 = sum / 5.0;
        }
        ASSERT_TRUE(app->AppendRow(t0 + i * 60'000'000LL, price, int64_t(100 + i), ma5, ma10).ok());
    }
    ASSERT_TRUE(app->Close().ok());
    ForceMerge(conn, "t");
    auto r = conn.Select("t");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 5u);
    EXPECT_EQ(r->ColumnCount(), 5u);
    EXPECT_EQ(double(r->Row(4)["ma5"]), 102.0);
}
