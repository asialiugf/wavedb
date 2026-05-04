#include <gtest/gtest.h>

#include <cstdio>

#include "wavedb/connection.h"
#include "wavedb/database.h"
#include "wavedb/types.h"

using namespace wavedb;

class EngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/wavedb_engine_test_XXXXXX";
        tmpdir_ = ::mkdtemp(tmpl);
    }
    void TearDown() override {
        {
            int rc = std::system(("rm -rf " + tmpdir_).c_str());
            (void)rc;
        };
    }
    std::string tmpdir_;
    static constexpr int64_t base_ts = 1767264600000000LL;
};

TEST_F(EngineTest, CreateTableAndQuery) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("ticks");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("price", ColumnType::FLOAT);
    schema.AddColumn("volume", ColumnType::INT);
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
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::FLOAT);
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
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::INT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("range_test");
    ASSERT_TRUE(app.ok());
    for (int i = 0; i < 5; ++i) app->AppendRow(base_ts + i * 60'000'000LL, int64_t(i));
    ASSERT_TRUE(app->Close().ok());

    auto r = conn.Select("range_test", {"*"}, base_ts + 60'000'000LL, base_ts + 180'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
}

// ── Select 参数组合测试 ──────────────────────

// 辅助：建表 + 写入 5 行 (ts + i*1min, 100+i, 1000+i)
struct SelectFixture {
    std::string dir;
    static constexpr int64_t ts0 = 1767264600000000LL;  // 2026-01-01 10:50:00

    void SetUp(const std::string& tmpdir) {
        dir = tmpdir;
        auto db = WaveDB::Open(dir);
        Connection conn(*db);
        TableSchema s("t");
        s.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
        s.AddColumn("val", ColumnType::INT);
        s.AddColumn("tag", ColumnType::FLOAT);
        conn.CreateTable(s);
        auto app = conn.CreateAppender("t");
        for (int i = 0; i < 5; ++i) app->AppendRow(ts0 + i * 60'000'000LL, int64_t(100 + i), double(1.0 + i * 0.1));
        app->Close();
    }
};

TEST_F(EngineTest, SelectAllColumnsDefault) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t");  // 默认 {"*"}
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 3u);
    EXPECT_EQ(r->RowCount(), 5u);
    EXPECT_EQ(r->column_names[0], "ts");
    EXPECT_EQ(r->column_names[1], "val");
    EXPECT_EQ(r->column_names[2], "tag");
}

TEST_F(EngineTest, SelectAllColumnsStar) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"*"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 3u);
    EXPECT_EQ(r->RowCount(), 5u);
}

TEST_F(EngineTest, SelectProjection) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"ts", "val"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 2u);
    EXPECT_EQ(r->column_names[0], "ts");
    EXPECT_EQ(r->column_names[1], "val");
    // 数据列校验
    EXPECT_EQ(std::get<int64_t>(r->rows[2][1]), int64_t(102));
}

TEST_F(EngineTest, SelectSingleColumn) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"tag"});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 1u);
    EXPECT_EQ(r->column_names[0], "tag");
    EXPECT_DOUBLE_EQ(std::get<double>(r->rows[3][0]), 1.3);
}

TEST_F(EngineTest, SelectFromTs) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // ts >= ts0 + 2min → row 2,3,4
    auto r = conn.Select("t", {"*"}, fx.ts0 + 120'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
    EXPECT_EQ(std::get<int64_t>(r->rows[0][0]), fx.ts0 + 120'000'000LL);
}

TEST_F(EngineTest, SelectToTs) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // ts <= ts0 + 1min → row 0,1
    auto r = conn.Select("t", {"*"}, 0, fx.ts0 + 60'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 2u);
}

TEST_F(EngineTest, SelectFromToRange) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // ts0+1min <= ts <= ts0+3min → row 1,2,3
    auto r = conn.Select("t", {"*"}, fx.ts0 + 60'000'000LL, fx.ts0 + 180'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
}

TEST_F(EngineTest, SelectFromToEmptyRange) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // ts >= ts0+10min → 空
    auto r = conn.Select("t", {"*"}, fx.ts0 + 600'000'000LL, fx.ts0 + 700'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 0u);
}

TEST_F(EngineTest, SelectLimit) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"*"}, 0, 0, 2);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 2u);
    // 取尾部：最后 2 行是 row 3,4
    EXPECT_EQ(std::get<int64_t>(r->rows[0][0]), fx.ts0 + 180'000'000LL);
}

TEST_F(EngineTest, SelectLimitLargerThanRows) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"*"}, 0, 0, 100);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 5u);  // 不超过实际行数
}

TEST_F(EngineTest, SelectLimitZero) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"*"}, 0, 0, 0);  // limit=0 → 全返
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 5u);
}

TEST_F(EngineTest, SelectProjectionAndFilter) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // ts 不在选中列中，但仍能按 ts 过滤
    auto r = conn.Select("t", {"val"}, fx.ts0 + 120'000'000LL, fx.ts0 + 300'000'000LL);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 1u);
    EXPECT_EQ(r->column_names[0], "val");
    EXPECT_EQ(r->RowCount(), 3u);
}

TEST_F(EngineTest, SelectAllParams) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    // projection + 时间范围 + limit
    auto r = conn.Select(
        "t", {"ts", "tag"},
        fx.ts0 + 60'000'000LL,   // from
        fx.ts0 + 240'000'000LL,  // to
        2);                      // limit
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 2u);
    EXPECT_EQ(r->RowCount(), 2u);  // row 3,4 (limit 取尾部)
}

TEST_F(EngineTest, SelectNonexistentTable) {
    auto db = WaveDB::Open(tmpdir_);
    Connection conn(*db);
    auto r = conn.Select("nope");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status.code(), StatusCode::NOT_FOUND);
}

TEST_F(EngineTest, SelectNonexistentColumn) {
    SelectFixture fx;
    fx.SetUp(tmpdir_);
    auto db = WaveDB::Open(fx.dir);
    Connection conn(*db);
    auto r = conn.Select("t", {"nope"});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status.code(), StatusCode::NOT_FOUND);
}

TEST_F(EngineTest, SelectEmptyTable) {
    auto db = WaveDB::Open(tmpdir_);
    Connection conn(*db);
    TableSchema s("empty");
    s.AddColumn("ts", ColumnType::TIMESTAMP);
    conn.CreateTable(s);
    auto r = conn.Select("empty");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 0u);
    EXPECT_EQ(r->ColumnCount(), 1u);
}

// ── 原有测试 ────────────────────────────────

TEST_F(EngineTest, ReadOnly) {
    {
        auto db = WaveDB::Open(tmpdir_);
        ASSERT_TRUE(db.ok());
        Connection conn(*db);
        TableSchema schema("ro_test");
        schema.AddColumn("ts", ColumnType::TIMESTAMP);
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

// ── ALTER TABLE 测试 ──────────────────────

TEST_F(EngineTest, AlterTableAddColumn) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    // 建表，写入 2 行（2 列：ts, val）
    TableSchema schema("alter_test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("val", ColumnType::INT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("alter_test");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, int64_t(100));
    app->AppendRow(base_ts + 60'000'000LL, int64_t(200));
    ASSERT_TRUE(app->Close().ok());

    // ALTER TABLE ADD FIELD price FLOAT
    ASSERT_TRUE(conn.AddColumn("alter_test", "price", ColumnType::FLOAT).ok());

    // 再写入 1 行（3 列：ts, val, price）
    auto app2 = conn.CreateAppender("alter_test");
    ASSERT_TRUE(app2.ok());
    app2->AppendRow(base_ts + 120'000'000LL, int64_t(300), 3.14);
    ASSERT_TRUE(app2->Close().ok());

    // 验证旧 Part（2 行）中 price 列为默认值 0.0
    auto r = conn.Select("alter_test");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 3u);
    EXPECT_EQ(r->ColumnCount(), 3u);
    EXPECT_EQ(r->column_names[2], "price");
    // 前两行：price = 0.0
    EXPECT_DOUBLE_EQ(std::get<double>(r->rows[0][2]), 0.0);
    EXPECT_DOUBLE_EQ(std::get<double>(r->rows[1][2]), 0.0);
    // 第三行：price = 3.14
    EXPECT_DOUBLE_EQ(std::get<double>(r->rows[2][2]), 3.14);
}

TEST_F(EngineTest, AlterTableDropColumn) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    // 建表（3 列：ts, price, volume）
    TableSchema schema("drop_test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("price", ColumnType::FLOAT);
    schema.AddColumn("volume", ColumnType::INT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("drop_test");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, 100.5, int64_t(1000));
    ASSERT_TRUE(app->Close().ok());

    // 删除 price 列
    ASSERT_TRUE(conn.DropColumn("drop_test", "price").ok());

    // 查询：确认只有 2 列
    auto r = conn.Select("drop_test");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->ColumnCount(), 2u);
    EXPECT_EQ(r->column_names[0], "ts");
    EXPECT_EQ(r->column_names[1], "volume");

    // 写入新行（只有 2 列）
    auto app2 = conn.CreateAppender("drop_test");
    ASSERT_TRUE(app2.ok());
    app2->AppendRow(base_ts + 60'000'000LL, int64_t(2000));
    ASSERT_TRUE(app2->Close().ok());

    auto r2 = conn.Select("drop_test");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2->RowCount(), 2u);
}

TEST_F(EngineTest, AlterTableAddColumnNonexistentTable) {
    auto db = WaveDB::Open(tmpdir_);
    Connection conn(*db);
    auto s = conn.AddColumn("nope", "x", ColumnType::INT);
    EXPECT_EQ(s.code(), StatusCode::NOT_FOUND);
}

TEST_F(EngineTest, AlterTableDropColumnNonexistentColumn) {
    auto db = WaveDB::Open(tmpdir_);
    Connection conn(*db);
    TableSchema schema("t");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    ASSERT_TRUE(conn.CreateTable(schema).ok());
    auto s = conn.DropColumn("t", "nope");
    EXPECT_EQ(s.code(), StatusCode::NOT_FOUND);
}

TEST_F(EngineTest, AlterTableReadOnly) {
    {
        auto db = WaveDB::Open(tmpdir_);
        Connection conn(*db);
        TableSchema schema("ro_alter");
        schema.AddColumn("ts", ColumnType::TIMESTAMP);
        ASSERT_TRUE(conn.CreateTable(schema).ok());
    }
    {
        auto db = WaveDB::Open(tmpdir_, {.read_only = true});
        ASSERT_TRUE(db.ok());
        Connection conn(*db);
        EXPECT_FALSE(conn.AddColumn("ro_alter", "x", ColumnType::INT).ok());
        EXPECT_FALSE(conn.DropColumn("ro_alter", "ts").ok());
    }
}

// ── UpdateColumn 测试 ──────────────────────

TEST_F(EngineTest, UpdateColumnBackfill) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    // 建表 + 写入 3 行（ts, val）
    TableSchema schema("upd");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("val", ColumnType::INT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    auto app = conn.CreateAppender("upd");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, int64_t(10));
    app->AppendRow(base_ts + 60'000'000LL, int64_t(20));
    app->AppendRow(base_ts + 120'000'000LL, int64_t(30));
    ASSERT_TRUE(app->Close().ok());

    // ADD COLUMN extra FLOAT
    ASSERT_TRUE(conn.AddColumn("upd", "extra", ColumnType::FLOAT).ok());

    // 验证旧行 extra 为 0
    auto r0 = conn.Select("upd");
    ASSERT_TRUE(r0.ok());
    EXPECT_DOUBLE_EQ(std::get<double>(r0->rows[0][2]), 0.0);

    // UpdateColumn: 用 vector 全量更新 extra
    ASSERT_TRUE(conn.UpdateColumn("upd", "extra", {Value(1.1), Value(2.2), Value(3.3)}).ok());

    // 验证更新后值
    auto r1 = conn.Select("upd");
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1->RowCount(), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(r1->rows[0][2]), 1.1);
    EXPECT_DOUBLE_EQ(std::get<double>(r1->rows[1][2]), 2.2);
    EXPECT_DOUBLE_EQ(std::get<double>(r1->rows[2][2]), 3.3);

    // 其他列不变
    EXPECT_EQ(std::get<int64_t>(r1->rows[0][1]), int64_t(10));
    EXPECT_EQ(std::get<int64_t>(r1->rows[2][1]), int64_t(30));
}

TEST_F(EngineTest, UpdateColumnCountMismatch) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("upd2");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);
    schema.AddColumn("val", ColumnType::INT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());
    auto app = conn.CreateAppender("upd2");
    app->AppendRow(base_ts, int64_t(1));
    app->Close();

    // values 数量与行数不匹配
    auto s = conn.UpdateColumn("upd2", "val", base_ts, base_ts, {int64_t(1), int64_t(2)});
    EXPECT_EQ(s.code(), StatusCode::INVALID_ARGUMENT);
}

TEST_F(EngineTest, RowViewAccess) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("rv");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("val", ColumnType::FLOAT);
    ASSERT_TRUE(conn.CreateTable(schema).ok());
    auto app = conn.CreateAppender("rv");
    app->AppendRow(base_ts, 100.5);
    app->AppendRow(base_ts + 60'000'000LL, 200.5);
    app->Close();

    auto r = conn.Select("rv");
    ASSERT_TRUE(r.ok());

    // Row(0) — Cell 隐式转换到 int64_t/double
    auto r0 = r->Row(0);
    EXPECT_EQ(int64_t(r0["ts"]), base_ts);
    EXPECT_DOUBLE_EQ(double(r0["val"]), 100.5);

    // Row("first") == Row(0)
    auto rf = r->Row("first");
    EXPECT_EQ(int64_t(rf["ts"]), base_ts);

    // Row("last")
    auto rl = r->Row("last");
    EXPECT_EQ(int64_t(rl["ts"]), base_ts + 60'000'000LL);
    EXPECT_DOUBLE_EQ(double(rl["val"]), 200.5);
}
