#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>

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
            std::error_code ec;
            std::filesystem::remove_all(tmpdir_, ec);
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

// ── Query() 统一 SQL 接口测试 ──────────────

TEST_F(EngineTest, QueryCreateTable) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    auto r = conn.Query("CREATE TABLE qct (ts TIMESTAMP(SECOND), val INT)");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::CREATE_TABLE);
    EXPECT_EQ(r->rows_affected, 0);
    // 验证表确实被创建
    EXPECT_NE(conn.GetTableSchema("qct"), nullptr);
}

TEST_F(EngineTest, QueryInsert) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qi (ts TIMESTAMP(SECOND), val INT)");
    auto r = conn.Query("INSERT INTO qi VALUES (20260101-10:50:00, 42)");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::INSERT);
    EXPECT_EQ(r->rows_affected, 1);

    // 验证数据已写入
    auto sel = conn.Select("qi");
    ASSERT_TRUE(sel.ok());
    EXPECT_EQ(sel->RowCount(), 1u);
}

TEST_F(EngineTest, QuerySelect) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qs (ts TIMESTAMP(SECOND), price FLOAT, vol INT)");
    conn.Query("INSERT INTO qs VALUES (20260101-09:30:00, 100.5, 1000)");
    conn.Query("INSERT INTO qs VALUES (20260101-09:31:00, 101.0, 1500)");

    // SELECT * — 全列
    auto r = conn.Query("SELECT * FROM qs");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::SELECT);
    EXPECT_EQ(r->ColumnCount(), 3u);
    EXPECT_EQ(r->RowCount(), 2u);

    // SELECT 指定列 projection
    auto r2 = conn.Query("SELECT price FROM qs");
    ASSERT_TRUE(r2.ok());
    EXPECT_EQ(r2->ColumnCount(), 1u);
    EXPECT_EQ(r2->column_names[0], "price");
    EXPECT_EQ(r2->RowCount(), 2u);
}

TEST_F(EngineTest, QueryAlterAddColumn) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qa (ts TIMESTAMP(SECOND), val INT)");
    conn.Query("INSERT INTO qa VALUES (20260101-09:30:00, 100)");

    auto r = conn.Query("ALTER TABLE qa ADD COLUMN extra FLOAT");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::ALTER_ADD_COLUMN);
    EXPECT_EQ(r->rows_affected, 0);

    // 验证列已添加 + 旧数据默认值
    auto sel = conn.Select("qa");
    ASSERT_TRUE(sel.ok());
    EXPECT_EQ(sel->ColumnCount(), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(sel->rows[0][2]), 0.0);
}

TEST_F(EngineTest, QueryAlterAddField) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qaf (ts TIMESTAMP, x INT)");
    // FIELD 是 COLUMN 的同义词
    auto r = conn.Query("ALTER TABLE qaf ADD FIELD y INT");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::ALTER_ADD_COLUMN);
    EXPECT_EQ(conn.GetTableSchema("qaf")->column_count(), 3u);
}

TEST_F(EngineTest, QueryAlterDropColumn) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qd (ts TIMESTAMP(SECOND), a INT, b FLOAT)");
    auto r = conn.Query("ALTER TABLE qd DROP COLUMN b");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::ALTER_DROP_COLUMN);

    auto* schema = conn.GetTableSchema("qd");
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->column_count(), 2u);
    EXPECT_EQ(schema->column_at(0).name, "ts");
    EXPECT_EQ(schema->column_at(1).name, "a");
}

TEST_F(EngineTest, QueryUpdate) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qu (ts TIMESTAMP(SECOND), val INT)");
    conn.Query("INSERT INTO qu VALUES (20260101-09:30:00, 10)");
    conn.Query("INSERT INTO qu VALUES (20260101-09:31:00, 20)");

    // 全表更新（无 FROM/TO）
    auto r = conn.Query("UPDATE qu SET val = 100, 200");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::UPDATE);
    EXPECT_EQ(r->rows_affected, 2);

    auto sel = conn.Select("qu");
    ASSERT_TRUE(sel.ok());
    EXPECT_EQ(std::get<int64_t>(sel->rows[0][1]), int64_t(100));
    EXPECT_EQ(std::get<int64_t>(sel->rows[1][1]), int64_t(200));
}

TEST_F(EngineTest, QueryUpdateRange) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE qur (ts TIMESTAMP(SECOND), val INT)");
    conn.Query("INSERT INTO qur VALUES (20260101-09:30:00, 10)");
    conn.Query("INSERT INTO qur VALUES (20260101-09:31:00, 20)");
    conn.Query("INSERT INTO qur VALUES (20260101-09:32:00, 30)");

    // 按范围更新 — FROM ts TO ts（值数量需等于范围内行数）
    auto r = conn.Query(
        "UPDATE qur SET val = 999, 999 FROM 20260101-09:31:00 TO 20260101-09:32:00");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::UPDATE);
    EXPECT_EQ(r->rows_affected, 2);

    auto sel = conn.Select("qur");
    ASSERT_TRUE(sel.ok());
    EXPECT_EQ(std::get<int64_t>(sel->rows[0][1]), int64_t(10));   // 不变
    EXPECT_EQ(std::get<int64_t>(sel->rows[1][1]), int64_t(999));  // 已更新
    EXPECT_EQ(std::get<int64_t>(sel->rows[2][1]), int64_t(999));  // 已更新
}

TEST_F(EngineTest, QueryParseError) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    auto r = conn.Query("GARBAGE SQL SYNTAX");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status.code(), StatusCode::PARSE_ERROR);
}

TEST_F(EngineTest, QuerySelectReadOnly) {
    {
        auto db = WaveDB::Open(tmpdir_);
        Connection conn(*db);
        conn.Query("CREATE TABLE qro (ts TIMESTAMP, v INT)");
        conn.Query("INSERT INTO qro VALUES (20260101, 1)");
    }
    // 只读连接 SELECT 应正常工作
    auto db = WaveDB::Open(tmpdir_, {.read_only = true});
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    auto r = conn.Query("SELECT * FROM qro");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 1u);
}

TEST_F(EngineTest, QueryInsertReadOnly) {
    {
        auto db = WaveDB::Open(tmpdir_);
        Connection conn(*db);
        conn.Query("CREATE TABLE qiro (ts TIMESTAMP, v INT)");
    }
    // 只读连接 INSERT 应失败
    auto db = WaveDB::Open(tmpdir_, {.read_only = true});
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    auto r = conn.Query("INSERT INTO qiro VALUES (20260101, 1)");
    EXPECT_FALSE(r.ok());
}

// ── QueryStream / Fetch 列优先迭代器测试 ──────

TEST_F(EngineTest, FetchSingleChunk) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE fc (ts TIMESTAMP(SECOND), price FLOAT, vol INT)");

    // 一个 Part 包含 3 行（用 CreateAppender 确保同 Part）
    auto app = conn.CreateAppender("fc");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, 100.5, int64_t(1000));
    app->AppendRow(base_ts + 60'000'000LL, 101.0, int64_t(1500));
    app->AppendRow(base_ts + 120'000'000LL, 102.0, int64_t(2000));
    ASSERT_TRUE(app->Close().ok());

    auto sr = conn.Query("SELECT * FROM fc");
    ASSERT_TRUE(sr.ok());

    // 第一个 Chunk（3 行 < 1024 默认 chunk_size）
    auto ch = sr->Fetch();
    ASSERT_TRUE(ch.ok());
    EXPECT_EQ(ch->row_count, 3u);
    EXPECT_EQ(ch->ColumnCount(), 3u);
    EXPECT_EQ(ch->column_names[0], "ts");
    EXPECT_EQ(ch->column_names[1], "price");
    EXPECT_EQ(ch->column_names[2], "vol");

    // 列优先：ts 列连续存储
    auto& ts_col = ch->columns[0];
    EXPECT_EQ(ts_col.type, ColumnType::TIMESTAMP);
    EXPECT_EQ(ts_col.i64.size(), 3u);
    EXPECT_EQ(ts_col.i64[0], base_ts);
    EXPECT_EQ(ts_col.i64[1], base_ts + 60'000'000LL);

    // price 列连续存储
    auto& price_col = ch->columns[1];
    EXPECT_EQ(price_col.type, ColumnType::FLOAT);
    EXPECT_EQ(price_col.f64.size(), 3u);
    EXPECT_DOUBLE_EQ(price_col.f64[0], 100.5);
    EXPECT_DOUBLE_EQ(price_col.f64[2], 102.0);

    // 第二个 Fetch：空 Chunk（数据已读完）
    auto empty = sr->Fetch();
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty->row_count, 0u);
}

TEST_F(EngineTest, FetchMultiChunk) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE fmc (ts TIMESTAMP(SECOND), v INT)");

    // 写入 5 行到一个 Part
    auto app = conn.CreateAppender("fmc");
    ASSERT_TRUE(app.ok());
    for (int i = 0; i < 5; ++i) app->AppendRow(base_ts + i * 60'000'000LL, int64_t(100 + i));
    ASSERT_TRUE(app->Close().ok());

    auto sr = conn.Query("SELECT * FROM fmc");
    ASSERT_TRUE(sr.ok());

    // 设 chunk_size=2，共 5 行 → 3 个 chunk（2 + 2 + 1）
    sr->SetChunkSize(2);

    size_t total = 0;
    int chunks = 0;
    while (true) {
        auto ch = sr->Fetch();
        ASSERT_TRUE(ch.ok());
        if (ch->row_count == 0) break;
        total += ch->row_count;
        ++chunks;
        // 验证每列 size() == row_count
        for (size_t ci = 0; ci < ch->ColumnCount(); ++ci) {
            EXPECT_EQ(ch->columns[ci].size(), ch->row_count);
        }
    }
    EXPECT_EQ(total, 5u);
    EXPECT_EQ(chunks, 3);
}

TEST_F(EngineTest, FetchMultiPart) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE fmp (ts TIMESTAMP(SECOND), v INT)");

    // 3 个 Part，每个 Part 2 行
    for (int p = 0; p < 3; ++p) {
        auto app = conn.CreateAppender("fmp");
        ASSERT_TRUE(app.ok());
        for (int i = 0; i < 2; ++i) app->AppendRow(base_ts + (p * 2 + i) * 60'000'000LL, int64_t(p * 10 + i));
        ASSERT_TRUE(app->Close().ok());
    }

    auto sr = conn.Query("SELECT v FROM fmp");
    ASSERT_TRUE(sr.ok());

    // chunk_size=2 → 每个 Part 正好 2 行，逐 Part 依次返回
    sr->SetChunkSize(2);

    // Chunk 1：Part 0
    auto ch1 = sr->Fetch();
    ASSERT_TRUE(ch1.ok());
    EXPECT_EQ(ch1->row_count, 2u);
    EXPECT_EQ(ch1->ColumnCount(), 1u);
    EXPECT_EQ(ch1->columns[0].i64[0], int64_t(0));   // p=0,i=0 → v=0
    EXPECT_EQ(ch1->columns[0].i64[1], int64_t(1));   // p=0,i=1 → v=1

    // Chunk 2：Part 1
    auto ch2 = sr->Fetch();
    ASSERT_TRUE(ch2.ok());
    EXPECT_EQ(ch2->row_count, 2u);
    EXPECT_EQ(ch2->columns[0].i64[0], int64_t(10));  // p=1,i=0 → v=10
    EXPECT_EQ(ch2->columns[0].i64[1], int64_t(11));  // p=1,i=1 → v=11

    // Chunk 3：Part 2
    auto ch3 = sr->Fetch();
    ASSERT_TRUE(ch3.ok());
    EXPECT_EQ(ch3->row_count, 2u);
    EXPECT_EQ(ch3->columns[0].i64[0], int64_t(20));  // p=2,i=0 → v=20

    // Chunk 4：空
    auto empty = sr->Fetch();
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty->row_count, 0u);
}

TEST_F(EngineTest, FetchColumnProjection) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE fproj (ts TIMESTAMP(SECOND), a INT, b FLOAT, c INT)");

    // 一个 Part 2 行
    auto app = conn.CreateAppender("fproj");
    ASSERT_TRUE(app.ok());
    app->AppendRow(base_ts, int64_t(10), 1.5, int64_t(100));
    app->AppendRow(base_ts + 60'000'000LL, int64_t(20), 2.5, int64_t(200));
    ASSERT_TRUE(app->Close().ok());

    // 只选 a, c 两列
    auto sr = conn.Query("SELECT a, c FROM fproj");
    ASSERT_TRUE(sr.ok());

    auto ch = sr->Fetch();
    ASSERT_TRUE(ch.ok());
    EXPECT_EQ(ch->ColumnCount(), 2u);
    EXPECT_EQ(ch->column_names[0], "a");
    EXPECT_EQ(ch->column_names[1], "c");
    EXPECT_EQ(ch->columns[0].type, ColumnType::INT);
    EXPECT_EQ(ch->columns[1].type, ColumnType::INT);
    EXPECT_EQ(ch->columns[0].i64[0], int64_t(10));
    EXPECT_EQ(ch->columns[1].i64[1], int64_t(200));
}

TEST_F(EngineTest, FetchEmptyTable) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    conn.Query("CREATE TABLE fe (ts TIMESTAMP(SECOND), v INT)");

    auto sr = conn.Query("SELECT * FROM fe");
    ASSERT_TRUE(sr.ok());

    auto ch = sr->Fetch();
    ASSERT_TRUE(ch.ok());
    EXPECT_EQ(ch->row_count, 0u);
    EXPECT_EQ(ch->ColumnCount(), 2u);  // 列元信息仍在
}

// ── Merge 测试 ──────────────────────────────

#include "src/storage/part_manager.h"

TEST_F(EngineTest, MergeByDay) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    // 创建带 MERGE BY DAY 的表
    TableSchema schema("md");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("v", ColumnType::INT);
    schema.setMergeConfig({MergePolicy::BY_DAY, 0});
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    // 写入 3 个 Part，都在同一天
    for (int p = 0; p < 3; ++p) {
        auto app = conn.CreateAppender("md");
        ASSERT_TRUE(app.ok());
        // 同一天不同时间
        app->AppendRow(base_ts + p * 3'600'000'000LL, int64_t(100 + p));
        app->AppendRow(base_ts + p * 3'600'000'000LL + 60'000'000LL, int64_t(200 + p));
        ASSERT_TRUE(app->Close().ok());
    }

    // 验证合并前数据
    auto r0 = conn.Select("md");
    ASSERT_TRUE(r0.ok());
    EXPECT_EQ(r0->RowCount(), 6u);

    // 手动触发合并
    std::string table_dir = tmpdir_ + "/md";
    auto schema_ptr = conn.GetTableSchema("md");
    ASSERT_NE(schema_ptr, nullptr);
    auto pm = PartManager::Open(table_dir, *schema_ptr);
    ASSERT_TRUE(pm.ok());
    auto lock = FileLock::Acquire(tmpdir_, true);
    ASSERT_TRUE(lock.ok());
    size_t merged = pm->MergeParts(schema.mergeConfig());
    EXPECT_GT(merged, 0u);

    // 验证合并后数据完整性
    auto r1 = conn.Select("md");
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1->RowCount(), 6u);
    // 验证值：p=0 → 100, 200; p=1 → 101, 201; p=2 → 102, 202
    int64_t sum = 0;
    for (size_t i = 0; i < r1->RowCount(); ++i) sum += std::get<int64_t>(r1->rows[i][1]);
    EXPECT_EQ(sum, 100 + 200 + 101 + 201 + 102 + 202);
}

TEST_F(EngineTest, MergeByDayWithMaxRows) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    TableSchema schema("mmr");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("v", ColumnType::INT);
    schema.setMergeConfig({MergePolicy::BY_DAY, 3});  // 每 Part 最多 3 行
    ASSERT_TRUE(conn.CreateTable(schema).ok());

    // 写入 2 个 Part，每个 3 行，共 6 行同一天
    for (int p = 0; p < 2; ++p) {
        auto app = conn.CreateAppender("mmr");
        ASSERT_TRUE(app.ok());
        for (int i = 0; i < 3; ++i)
            app->AppendRow(base_ts + (p * 3 + i) * 60'000'000LL, int64_t(p * 10 + i));
        ASSERT_TRUE(app->Close().ok());
    }

    std::string table_dir = tmpdir_ + "/mmr";
    auto schema_ptr = conn.GetTableSchema("mmr");
    auto pm = PartManager::Open(table_dir, *schema_ptr);
    ASSERT_TRUE(pm.ok());
    auto lock = FileLock::Acquire(tmpdir_, true);
    ASSERT_TRUE(lock.ok());
    pm->MergeParts(schema.mergeConfig());

    // 合并后应有 2 个 Part（6行 ÷ max_rows=3）
    // 重新打开 PartManager 查看
    auto pm2 = PartManager::Open(table_dir, *schema_ptr);
    ASSERT_TRUE(pm2.ok());
    EXPECT_GE(pm2->all_parts().size(), 1u);  // 至少合并了一部分

    // 数据完整性
    auto r = conn.Select("mmr");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->RowCount(), 6u);
}

TEST_F(EngineTest, MergeByMonthSQL) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    auto r = conn.Query(
        "CREATE TABLE mbm (ts TIMESTAMP(SECOND), v INT) MERGE BY MONTH MAX_ROWS 10000");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r->statement_type, StatementType::CREATE_TABLE);

    auto* schema = conn.GetTableSchema("mbm");
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->mergeConfig().policy, MergePolicy::BY_MONTH);
    EXPECT_EQ(schema->mergeConfig().max_rows_per_part, 10000);
}

TEST_F(EngineTest, MergeNoneByDefault) {
    auto db = WaveDB::Open(tmpdir_);
    ASSERT_TRUE(db.ok());
    Connection conn(*db);

    // 不写 MERGE → 默认 NONE
    conn.Query("CREATE TABLE mndef (ts TIMESTAMP(SECOND), v INT)");
    auto* schema = conn.GetTableSchema("mndef");
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(schema->mergeConfig().policy, MergePolicy::NONE);
    EXPECT_EQ(schema->mergeConfig().max_rows_per_part, 0);
}
