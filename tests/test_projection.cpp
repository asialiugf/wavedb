// 列投影与 WHERE 过滤综合测试：SELECT 列投影、WHERE 时间过滤、不同 TS 精度、LIMIT。
// 用法: cmake -B build && cmake --build build && ./build/test_projection

#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/status.h"
#include "wavedb/types.h"
#include "test_harness.h"

using namespace wavedb;

static constexpr int64_t t0 = 1767264600000000LL;  // 2026-01-01 10:50:00 µs

// ===================================================================
// 辅助
// ===================================================================

struct ProjFixture {
    std::string dir;

    // 默认：4 列 5 行，TS SECOND
    void SetUp() {
        dir = MakeTempDir("wavedb_proj");
        auto db = WaveDB::Open(dir);
        CHECK_OK(db);
        Connection conn(*db);
        auto r = conn.Query("CREATE TABLE t (ts TIMESTAMP(SECOND), a INT, b FLOAT, c INT)");
        CHECK_OK(r);
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        for (int i = 0; i < 5; ++i)
            app->AppendRow(t0 + i * 60'000'000LL, int64_t(10 + i), double(1.5 + i), int64_t(100 + i * 10));
        CHECK_OK(app->Close());
    }

    void TearDown() { RemoveDir(dir); }
};

// ===================================================================
// 1. 基础列投影
// ===================================================================

static void test_two_column_projection() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT a, c FROM t");
    CHECK_OK(sr);
    CHECK_EQ(sr->ColumnCount(), 2u);
    CHECK_EQ(sr->column_names[0], std::string("a"));
    CHECK_EQ(sr->column_names[1], std::string("c"));
    CHECK_EQ(sr->RowCount(), 5u);
    CHECK_EQ(int64_t(sr->Row(0)["a"]), int64_t(10));

    fx.TearDown();
}

static void test_single_column() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT b FROM t");
    CHECK_OK(sr);
    CHECK_EQ(sr->ColumnCount(), 1u);
    CHECK_EQ(sr->RowCount(), 5u);
    CHECK_EQ(double(sr->Row(3)["b"]), 4.5);

    fx.TearDown();
}

static void test_select_star() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT * FROM t");
    CHECK_OK(sr);
    CHECK_EQ(sr->ColumnCount(), 4u);
    CHECK_EQ(sr->RowCount(), 5u);

    fx.TearDown();
}

static void test_nonexistent_column() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT nope FROM t");
    CHECK(!sr.ok());

    fx.TearDown();
}

// ===================================================================
// 2. WHERE ts >= ... AND ts <= ...（时间戳字面量，各种精度）
// ===================================================================

// SECOND 精度字面量
static void test_where_ts_second() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // ts ∈ [t0+1min, t0+3min] → 3 行
    auto sr = conn.Query(
        "SELECT a, b FROM t "
        "WHERE ts >= 20260101-10:51:00 AND ts <= 20260101-10:53:00");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 3u);
    CHECK_EQ(int64_t(sr->Row(0)["a"]), int64_t(11));
    CHECK_EQ(double(sr->Row(1)["b"]), 3.5);

    fx.TearDown();
}

// MINUTE 精度字面量（输入短于列精度，自动补零）
static void test_where_ts_minute() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // 20260101-10:50 补零 → 20260101-10:50:00 → 3 行 >= t0
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10:50");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 5u);  // 全部 >= t0

    fx.TearDown();
}

// HOUR 精度字面量
static void test_where_ts_hour() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // 20260101-10 → 2026-01-01 10:00:00 → 早于 t0(10:50:00)，全部匹配
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 5u);

    fx.TearDown();
}

// DAY 精度字面量
static void test_where_ts_day() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 5u);

    fx.TearDown();
}

// MILLI 精度字面量
static void test_where_ts_milli() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10:50:00-000");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 5u);

    fx.TearDown();
}

// MICRO 精度字面量
static void test_where_ts_micro() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10:50:00-000000");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 5u);

    fx.TearDown();
}

// ===================================================================
// 3. WHERE 仅下界 / 仅上界
// ===================================================================

static void test_where_ts_lower_only() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT c FROM t WHERE ts >= 20260101-10:52:00");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 3u);  // row 2,3,4
    CHECK_EQ(int64_t(sr->Row(0)["c"]), int64_t(120));

    fx.TearDown();
}

static void test_where_ts_upper_only() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query("SELECT ts, b FROM t WHERE ts <= 20260101-10:52:00");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 3u);  // row 0,1,2
    CHECK_EQ(int64_t(sr->Row(0)["ts"]), t0);

    fx.TearDown();
}

static void test_where_ts_empty() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // from_ts > to_ts 构成空范围（ts >= 10:52 且 ts <= 10:51 → 无行满足）
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10:52:00 AND ts <= 20260101-10:51:00");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 0u);
    CHECK_EQ(sr->ColumnCount(), 4u);

    fx.TearDown();
}

// ===================================================================
// 4. WHERE + LIMIT 组合
// ===================================================================

static void test_where_ts_with_limit() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto sr = conn.Query(
        "SELECT a, c FROM t WHERE ts >= 20260101-10:50:00 LIMIT 2");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 2u);  // 尾部 2 行 (row 3,4)
    CHECK_EQ(int64_t(sr->Row(0)["a"]), int64_t(13));
    CHECK_EQ(int64_t(sr->Row(1)["a"]), int64_t(14));

    fx.TearDown();
}

// ===================================================================
// 5. WHERE col >= val AND col <= val 语法（当前 parser 忽略列名，值始终映射为 TS 过滤）
// ===================================================================

static void test_where_style_int_value() {
    ProjFixture fx;
    fx.SetUp();
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // 语法兼容：列名忽略，数字值直接作为 from_ts/to_ts（微秒）
    auto sr = conn.Query(
        "SELECT a FROM t WHERE a >= 12 AND a <= 14");
    CHECK_OK(sr);
    // from_ts=12, to_ts=14 → 非常小的微秒值 → 所有数据行 ts >> 14 → 0 行
    CHECK_EQ(sr->RowCount(), 0u);

    fx.TearDown();
}

// ===================================================================
// 6. 精度自适应：CREATE TABLE TIMESTAMP(DAY) + WHERE 不同精度
// ===================================================================

// DAY 列，WHERE 输入微秒精度 → 截断到天
static void test_precision_truncate_fine_to_day() {
    std::string dir = MakeTempDir("wavedb_prec");
    {
        auto db = WaveDB::Open(dir);
        CHECK_OK(db);
        Connection conn(*db);

        auto ct = conn.Query("CREATE TABLE t (ts TIMESTAMP(DAY), v INT)");
        CHECK_OK(ct);
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        // 写入 20260101, 20260102 两天的数据（微秒值同 SECOND 精度）
        int64_t d1 = 1767264600000000LL;  // 2026-01-01 10:50:00 → truncated to 2026-01-01
        int64_t d2 = d1 + 86'400'000'000LL;  // 2026-01-02
        app->AppendRow(d1, int64_t(1));
        app->AppendRow(d1 + 3600'000'000LL, int64_t(2));  // same day, different hour
        app->AppendRow(d2, int64_t(3));
        CHECK_OK(app->Close());
    }

    auto db = WaveDB::Open(dir);
    CHECK_OK(db);
    Connection conn(*db);

    // WHERE 输入细于 DAY（SECOND 精度）→ 截断，20260101-10:50:00 → 20260101-00:00:00
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101-10:50:00 AND ts <= 20260101-23:59:59");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 2u);  // day 1 的两行

    RemoveDir(dir);
}

// DAY 列，WHERE 输入 DAY 精度（同精度）→ 不调整
static void test_precision_same_as_day() {
    std::string dir = MakeTempDir("wavedb_prec");
    {
        auto db = WaveDB::Open(dir);
        CHECK_OK(db);
        Connection conn(*db);

        auto ct = conn.Query("CREATE TABLE t (ts TIMESTAMP(DAY), v INT)");
        CHECK_OK(ct);
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        int64_t d1 = 1767264600000000LL;
        int64_t d2 = d1 + 86'400'000'000LL;
        int64_t d3 = d2 + 86'400'000'000LL;
        app->AppendRow(d1, int64_t(1));
        app->AppendRow(d2, int64_t(2));
        app->AppendRow(d3, int64_t(3));
        CHECK_OK(app->Close());
    }

    auto db = WaveDB::Open(dir);
    CHECK_OK(db);
    Connection conn(*db);

    // WHERE 输入同精度 → 直接比较
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts >= 20260101 AND ts <= 20260102");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 2u);  // 前两天

    RemoveDir(dir);
}

// MICRO 列，WHERE 输入 DAY 精度（粗于列）→ <= 扩展到 23:59:59.999999
static void test_precision_expand_coarse_in_micro() {
    std::string dir = MakeTempDir("wavedb_prec");
    {
        auto db = WaveDB::Open(dir);
        CHECK_OK(db);
        Connection conn(*db);

        auto ct = conn.Query("CREATE TABLE t (ts TIMESTAMP(MICRO), v INT)");
        CHECK_OK(ct);
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        // 2026-01-01 的 epoch 起点 + 不同偏移（都在同一天内）
        int64_t jan1 = 1767225600000000LL;  // 2026-01-01 00:00:00 UTC
        int64_t jan2 = jan1 + 86'400'000'000LL;  // 2026-01-02 00:00:00 UTC
        app->AppendRow(jan1 + 1LL * 3600'000'000LL, int64_t(1));   // 01:00
        app->AppendRow(jan1 + 12LL * 3600'000'000LL, int64_t(2));  // 12:00
        app->AppendRow(jan1 + 23LL * 3600'000'000LL, int64_t(3));  // 23:00
        app->AppendRow(jan2 + 1LL * 3600'000'000LL, int64_t(4));   // 次日 01:00
        CHECK_OK(app->Close());
    }

    auto db = WaveDB::Open(dir);
    CHECK_OK(db);
    Connection conn(*db);

    // WHERE ts <= 20260101（DAY 精度）→ 扩展到 2026-01-01 23:59:59.999999
    auto sr = conn.Query(
        "SELECT * FROM t "
        "WHERE ts <= 20260101");
    CHECK_OK(sr);
    CHECK_EQ(sr->RowCount(), 3u);  // 第 1 天的 3 行（次日那行排除）

    RemoveDir(dir);
}

// ===================================================================
// 入口
// ===================================================================

void run_tests() {
    test_two_column_projection();
    test_single_column();
    test_select_star();
    test_nonexistent_column();
    test_where_ts_second();
    test_where_ts_minute();
    test_where_ts_hour();
    test_where_ts_day();
    test_where_ts_milli();
    test_where_ts_micro();
    test_where_ts_lower_only();
    test_where_ts_upper_only();
    test_where_ts_empty();
    test_where_ts_with_limit();
    test_where_style_int_value();
    // 精度自适应
    test_precision_truncate_fine_to_day();
    test_precision_same_as_day();
    test_precision_expand_coarse_in_micro();
}

RUN_TESTS()
