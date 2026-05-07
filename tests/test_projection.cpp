// 列投影与 WHERE 过滤综合测试：SELECT 列投影、WHERE 时间过滤、不同 TS 精度、LIMIT。

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <gtest/gtest.h>

#include "src/storage/part_manager.h"
#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/status.h"
#include "wavedb/types.h"

using namespace wavedb;

static constexpr int64_t t0 = 1767264600000000LL;  // 2026-01-01 10:50:00 µs

static std::string MakeTempDir(const char* suffix) {
    std::string path = "/tmp/wavedb_proj_test_";
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

// ===================================================================
// 基础投影测试
// ===================================================================

class ProjTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = MakeTempDir("proj");
        auto db = WaveDB::Open(dir_);
        ASSERT_TRUE(db.ok());
        Connection conn(*db);
        ASSERT_TRUE(conn.Query("CREATE TABLE t (ts TIMESTAMP(SECOND), a INT, b FLOAT, c INT)").ok());
        auto app = conn.CreateAppender("t");
        ASSERT_TRUE(app.ok());
        for (int i = 0; i < 5; ++i)
            app->AppendRow(t0 + i * 60'000'000LL, int64_t(10 + i), double(1.5 + i), int64_t(100 + i * 10));
        ASSERT_TRUE(app->Close().ok());
        // Reader 只读 m_，合成 n_ → m_
        auto pm = PartManager::Open(dir_ + "/t", *conn.GetTableSchema("t"));
        if (pm.ok()) pm->MergeParts(conn.GetTableSchema("t")->mergeConfig());
    }
    void TearDown() override { RemoveDir(dir_); }
    std::string dir_;
};

TEST_F(ProjTest, TwoColumnProjection) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a, c FROM t");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->ColumnCount(), 2u);
    EXPECT_EQ(sr->column_names[0], "a");
    EXPECT_EQ(sr->column_names[1], "c");
    EXPECT_EQ(sr->RowCount(), 5u);
    EXPECT_EQ(int64_t(sr->Row(0)["a"]), int64_t(10));
}

TEST_F(ProjTest, SingleColumn) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT b FROM t");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->ColumnCount(), 1u);
    EXPECT_EQ(sr->RowCount(), 5u);
    EXPECT_EQ(double(sr->Row(3)["b"]), 4.5);
}

TEST_F(ProjTest, SelectStar) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->ColumnCount(), 4u);
    EXPECT_EQ(sr->RowCount(), 5u);
}

TEST_F(ProjTest, NonexistentColumn) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT nope FROM t");
    EXPECT_FALSE(sr.ok());
}

// ===================================================================
// WHERE ts >= / ts <= 各精度
// ===================================================================

TEST_F(ProjTest, WhereTsSecond) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a, b FROM t WHERE ts >= 20260101-10:51:00 AND ts <= 20260101-10:53:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 3u);
    EXPECT_EQ(int64_t(sr->Row(0)["a"]), int64_t(11));
    EXPECT_EQ(double(sr->Row(1)["b"]), 3.5);
}

TEST_F(ProjTest, WhereTsMinute) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 5u);
}

TEST_F(ProjTest, WhereTsHour) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 5u);
}

TEST_F(ProjTest, WhereTsDay) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 5u);
}

TEST_F(ProjTest, WhereTsMilli) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50:00-000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 5u);
}

TEST_F(ProjTest, WhereTsMicro) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50:00-000000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 5u);
}

// ===================================================================
// WHERE 仅下界 / 仅上界
// ===================================================================

TEST_F(ProjTest, WhereLowerOnly) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT c FROM t WHERE ts >= 20260101-10:52:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 3u);
    EXPECT_EQ(int64_t(sr->Row(0)["c"]), int64_t(120));
}

TEST_F(ProjTest, WhereUpperOnly) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT ts, b FROM t WHERE ts <= 20260101-10:52:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 3u);
    EXPECT_EQ(int64_t(sr->Row(0)["ts"]), t0);
}

TEST_F(ProjTest, WhereEmpty) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:52:00 AND ts <= 20260101-10:51:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 0u);
    EXPECT_EQ(sr->ColumnCount(), 4u);
}

TEST_F(ProjTest, WhereGtAndLt) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a FROM t WHERE ts > 20260101-10:51:00 AND ts < 20260101-10:53:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 1u);
    EXPECT_EQ(int64_t(sr->Row(0)["a"]), int64_t(12));
}

TEST_F(ProjTest, WhereLtUpperOnly) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a FROM t WHERE ts < 20260101-10:52:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    EXPECT_EQ(int64_t(sr->Row(0)["a"]), int64_t(10));
}

// ===================================================================
// WHERE + LIMIT
// ===================================================================

TEST_F(ProjTest, WhereTsWithLimit) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a, c FROM t WHERE ts >= 20260101-10:50:00 LIMIT 2");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    EXPECT_EQ(int64_t(sr->Row(0)["a"]), int64_t(13));
    EXPECT_EQ(int64_t(sr->Row(1)["a"]), int64_t(14));
}

TEST_F(ProjTest, WhereStyleIntValue) {
    auto db = WaveDB::Open(dir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT a FROM t WHERE a >= 12 AND a <= 14");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 0u);
}

// ===================================================================
// 精度自适应：>= 输入细于列 → 截断
// ===================================================================

class PrecisionTest : public ::testing::Test {
  protected:
    static std::string WriteData(const char* suffix, const char* createSql,
                                  const std::vector<std::pair<int64_t, int64_t>>& rows) {
        std::string dir = MakeTempDir(suffix);
        auto db = WaveDB::Open(dir);
        Connection conn(*db);
        conn.Query(createSql);
        auto app = conn.CreateAppender("t");
        for (auto& r : rows) app->AppendRow(r.first, r.second);
        app->Close();
        // Reader 只读 m_，合成 n_ → m_
        auto* schema = conn.GetTableSchema("t");
        auto pm = PartManager::Open(dir + "/t", *schema);
        if (pm.ok()) pm->MergeParts(schema->mergeConfig());
        return dir;
    }
};

TEST_F(PrecisionTest, TruncateFineToDay) {
    std::string dir = WriteData("prec_d", "CREATE TABLE t (ts TIMESTAMP(DAY), v INT)", {
        {1767264600000000LL, 1}, {1767264600000000LL + 3600'000'000LL, 2},
        {1767264600000000LL + 86'400'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50:00 AND ts <= 20260101-23:59:59");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, TruncateFineToHour) {
    std::string dir = WriteData("prec_h", "CREATE TABLE t (ts TIMESTAMP(HOUR), v INT)", {
        {1767225600000000LL, 1}, {1767225600000000LL + 3600'000'000LL, 2}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-00:30");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, TruncateFineToMinute) {
    std::string dir = WriteData("prec_min", "CREATE TABLE t (ts TIMESTAMP(MINUTE), v INT)", {
        {1767264600000000LL, 1}, {1767264600000000LL + 60'000'000LL, 2}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50:30");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, TruncateFineToSecond) {
    std::string dir = WriteData("prec_s", "CREATE TABLE t (ts TIMESTAMP(SECOND), v INT)", {
        {t0, 1}, {t0 + 1'000'000LL, 2}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101-10:50:00-500000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, SameAsDay) {
    std::string dir = WriteData("prec_eq", "CREATE TABLE t (ts TIMESTAMP(DAY), v INT)", {
        {1767264600000000LL, 1}, {1767264600000000LL + 86'400'000'000LL, 2},
        {1767264600000000LL + 2 * 86'400'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101 AND ts <= 20260102");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, CoarseInMicroLower) {
    std::string dir = WriteData("prec_cl", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {1767225600000000LL + 1LL * 3600'000'000LL, 1},
        {1767225600000000LL + 12LL * 3600'000'000LL, 2},
        {1767225600000000LL + 86'400'000'000LL + 1LL * 3600'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts >= 20260101");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 3u);
    RemoveDir(dir);
}

// ===================================================================
// 精度自适应：<= 扩展到周期末尾
// ===================================================================

TEST_F(PrecisionTest, ExpandDayInMicro) {
    std::string dir = WriteData("prec_ed", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {1767225600000000LL + 1LL * 3600'000'000LL, 1},
        {1767225600000000LL + 12LL * 3600'000'000LL, 2},
        {1767225600000000LL + 23LL * 3600'000'000LL, 3},
        {1767225600000000LL + 86'400'000'000LL + 1LL * 3600'000'000LL, 4}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 3u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, ExpandHourInMicro) {
    std::string dir = WriteData("prec_eh", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {1767225600000000LL + 30LL * 60'000'000LL, 1},
        {1767225600000000LL + 90LL * 60'000'000LL, 2},
        {1767225600000000LL + 150LL * 60'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101-01");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, ExpandMinuteInMicro) {
    std::string dir = WriteData("prec_em", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 30'000'000LL, 2}, {t0 + 60'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101-10:50");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, ExpandSecondInMicro) {
    std::string dir = WriteData("prec_es", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 500'000LL, 2}, {t0 + 1'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101-10:50:00");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, ExpandMilliInMicro) {
    std::string dir = WriteData("prec_ems", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 500LL, 2}, {t0 + 1'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101-10:50:00-000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, MicroUnchanged) {
    std::string dir = WriteData("prec_mc", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 500'000LL, 2}, {t0 + 1'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101-10:50:00-500000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

// ===================================================================
// 精度自适应：> / < 操作符
// ===================================================================

TEST_F(PrecisionTest, GtOperator) {
    std::string dir = WriteData("prec_gt", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 500'000LL, 2}, {t0 + 1'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts > 20260101-10:50:00-000");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, LtOperator) {
    std::string dir = WriteData("prec_lt", "CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)", {
        {t0, 1}, {t0 + 500'000LL, 2}, {t0 + 1'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts < 20260101-10:50:01");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}

TEST_F(PrecisionTest, ExpandDayInHourCol) {
    std::string dir = WriteData("prec_hd", "CREATE TABLE t (ts TIMESTAMP(HOUR), v INT)", {
        {1767225600000000LL, 1},
        {1767225600000000LL + 23LL * 3600'000'000LL, 2},
        {1767225600000000LL + 24LL * 3600'000'000LL, 3}});
    auto db = WaveDB::Open(dir);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);
    auto sr = conn.Query("SELECT * FROM t WHERE ts <= 20260101");
    ASSERT_TRUE(sr.ok());
    EXPECT_EQ(sr->RowCount(), 2u);
    RemoveDir(dir);
}
