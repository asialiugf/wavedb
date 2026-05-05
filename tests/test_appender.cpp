// Appender 独立测试：批量写入、缓冲、刷盘、数据校验。
// 用法: cmake -B build && cmake --build build && ./build/test_appender

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>

#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/status.h"
#include "wavedb/types.h"
#include "test_harness.h"

using namespace wavedb;

static constexpr int64_t t0 = 1767264600000000LL;

// ── 辅助 ──

struct AppenderFixture {
    std::string dir;

    void SetUp() {
        dir = MakeTempDir("wavedb_app");
        auto db = WaveDB::Open(dir);
        CHECK_OK(db);
        Connection conn(*db);

        auto ct = conn.Query("CREATE TABLE t (ts TIMESTAMP(SECOND), price FLOAT, vol INT)");
        CHECK_OK(ct);
    }

    void TearDown() { RemoveDir(dir); }
};

// ── 单行 Append + Close → 验证 ──

static void test_single_append() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    CHECK_OK(app->AppendRow(t0, 100.5, int64_t(500)));
    CHECK_EQ(app->buffered_rows(), 1u);
    CHECK_EQ(app->total_rows(), 1u);

    CHECK_OK(app->Close());

    // 验证数据
    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 1u);
    CHECK_EQ(int64_t(r->Row(0)["ts"]), t0);
    CHECK_EQ(double(r->Row(0)["price"]), 100.5);
    CHECK_EQ(int64_t(r->Row(0)["vol"]), int64_t(500));

    fx.TearDown();
}

// ── 多行变参 Append + Flush ──

static void test_variadic_append() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    // 变参模板：AppendRow(ts, price, vol)
    CHECK_OK(app->AppendRow(t0, 10.0, int64_t(100)));
    CHECK_OK(app->AppendRow(t0 + 60'000'000LL, 10.1, int64_t(110)));

    CHECK_OK(app->Flush());

    // 继续追加
    CHECK_OK(app->AppendRow(t0 + 120'000'000LL, 10.2, int64_t(120)));

    CHECK_OK(app->Close());
    CHECK_EQ(app->total_rows(), 3u);

    // 验证 3 行
    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 3u);
    CHECK_EQ(int64_t(r->Row(0)["ts"]), t0);
    CHECK_EQ(double(r->Row(1)["price"]), 10.1);
    CHECK_EQ(int64_t(r->Row(2)["vol"]), int64_t(120));

    fx.TearDown();
}

// ── 多批次写入（模拟高频写入）──

static void test_multiple_batches() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // 3 批次，每批 2 行
    for (int batch = 0; batch < 3; ++batch) {
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        for (int i = 0; i < 2; ++i) {
            int64_t ts = t0 + (batch * 2 + i) * 60'000'000LL;
            CHECK_OK(app->AppendRow(ts, double(100 + batch * 10 + i), int64_t(1000 + batch * 100 + i)));
        }
        CHECK_OK(app->Close());
        CHECK_EQ(app->total_rows(), 2u);
    }

    // 验证 6 行
    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 6u);

    fx.TearDown();
}

// ── 列数不匹配应报错 ──

static void test_column_count_mismatch() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    // 只给 2 个值（应为 3 列）
    auto s = app->AppendRow(t0, 1.0);
    CHECK(!s.ok());

    // 给 4 个值（应为 3 列）
    s = app->AppendRow(t0, 1.0, int64_t(1), int64_t(42));
    CHECK(!s.ok());

    // 正确行仍可写入
    CHECK_OK(app->AppendRow(t0, 1.0, int64_t(1)));
    CHECK_OK(app->Close());

    fx.TearDown();
}

// ── 类型不匹配应报错 ──

static void test_type_mismatch() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    // price 列应为 FLOAT，给 int64_t
    auto s = app->AppendRow(t0, int64_t(1), int64_t(1));
    CHECK(!s.ok());

    fx.TearDown();
}

// ── 空 Flush 是安全的 ──

static void test_empty_flush() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    // Flush 无数据时应无操作
    CHECK_OK(app->Flush());

    // 再写入
    CHECK_OK(app->AppendRow(t0, 1.0, int64_t(1)));
    CHECK_OK(app->Close());

    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 1u);

    fx.TearDown();
}

// ── Appender 计数器 ──

static void test_counter_state() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    CHECK_EQ(app->buffered_rows(), 0u);
    CHECK_EQ(app->total_rows(), 0u);

    CHECK_OK(app->AppendRow(t0, 1.0, int64_t(1)));
    CHECK_EQ(app->buffered_rows(), 1u);
    CHECK_EQ(app->total_rows(), 1u);

    CHECK_OK(app->Flush());
    CHECK_EQ(app->buffered_rows(), 0u);   // 缓冲已清空
    CHECK_EQ(app->total_rows(), 1u);      // 累计保持

    CHECK_OK(app->AppendRow(t0 + 1, 2.0, int64_t(2)));
    CHECK_OK(app->AppendRow(t0 + 2, 3.0, int64_t(3)));
    CHECK_EQ(app->buffered_rows(), 2u);
    CHECK_EQ(app->total_rows(), 3u);

    CHECK_OK(app->Close());

    fx.TearDown();
}

// ── 析构自动刷盘 ──

static void test_dtor_auto_flush() {
    AppenderFixture fx;
    fx.SetUp();

    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    {
        auto app = conn.CreateAppender("t");
        CHECK_OK(app);
        CHECK_OK(app->AppendRow(t0, 5.5, int64_t(99)));
        // app 出作用域 → 析构 → WritePart
    }

    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 1u);
    CHECK_EQ(double(r->Row(0)["price"]), 5.5);

    fx.TearDown();
}

// ── 计算列（MA5/MA10），和 Writer 逻辑一致 ──

static void test_computed_columns() {
    AppenderFixture fx;
    fx.SetUp();

    {
        auto db = WaveDB::Open(fx.dir);
        Connection conn(*db);

        // ALTER TABLE 加两列
        CHECK_OK(conn.AddColumn("t", "ma5", ColumnType::FLOAT));
        CHECK_OK(conn.AddColumn("t", "ma10", ColumnType::FLOAT));
    }

    // 用新 schema 重新打开（8 列）
    auto db = WaveDB::Open(fx.dir);
    CHECK_OK(db);
    Connection conn(*db);

    // 旧数据 0 行 — 直接追加 5 行（5 列值：ts, price, vol, ma5, ma10）
    auto app = conn.CreateAppender("t");
    CHECK_OK(app);

    // 模拟 Writer 逻辑：前 4 行 ma5/ma10 = 0，第 5 行开始计算
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
        if (closes.size() >= 10) {
            double sum = 0;
            for (size_t j = closes.size() - 10; j < closes.size(); ++j) sum += closes[j];
            ma10 = sum / 10.0;
        }

        CHECK_OK(app->AppendRow(t0 + i * 60'000'000LL, price, int64_t(100 + i), ma5, ma10));
    }
    CHECK_OK(app->Close());

    // 验证第 5 行 ma5 = (100+101+102+103+104)/5 = 102.0
    auto r = conn.Select("t");
    CHECK_OK(r);
    CHECK_EQ(r->RowCount(), 5u);
    CHECK_EQ(r->ColumnCount(), 5u);
    CHECK_EQ(double(r->Row(4)["ma5"]), 102.0);

    fx.TearDown();
}

// ── 入口 ──

void run_tests() {
    test_single_append();
    test_variadic_append();
    test_multiple_batches();
    test_column_count_mismatch();
    test_type_mismatch();
    test_empty_flush();
    test_counter_state();
    test_dtor_auto_flush();
    test_computed_columns();
}

RUN_TESTS()
