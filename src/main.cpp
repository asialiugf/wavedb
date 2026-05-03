#include <cassert>
#include <cstdlib>
#include <iostream>

#include "src/engine/connection.h"
#include "src/engine/wavedb.h"

using namespace wavedb;

static int64_t base_ts = 1767264600000000LL;  // 2026-01-01 09:30:00 UTC

// ---- 输出辅助 ----

static void PrintResult(const QueryResult& r) {
    // 表头
    for (size_t i = 0; i < r.ColumnCount(); ++i) {
        if (i > 0) std::cout << " | ";
        std::cout << r.column_names[i];
    }
    std::cout << "\n";

    // 行
    for (auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) std::cout << " | ";
            if (r.column_types[i] == ColumnType::kTimestamp) {
                std::cout << FormatTimestamp(std::get<int64_t>(row[i]), r.column_precisions[i]);
            } else if (r.column_types[i] == ColumnType::kFloat) {
                std::cout << std::get<double>(row[i]);
            } else {
                std::cout << std::get<int64_t>(row[i]);
            }
        }
        std::cout << "\n";
    }
}

// ---- 测试 ----

static void RunWriteTests(const std::string& data_dir) {
    auto db = WaveDB::Open(data_dir);
    assert(db.ok() && !db->read_only());
    Connection conn(*db);

    std::cout << "db path: " << db->path() << "\n\n";

    // === Test 1: CreateTable ===
    std::cout << "=== Test 1: CreateTable ===\n";
    TableSchema schema("ticks");
    schema.AddColumn("ts", ColumnType::kTimestamp, TimePrecision::SECOND);
    schema.AddColumn("price", ColumnType::kFloat);
    schema.AddColumn("volume", ColumnType::kInt);
    assert(conn.CreateTable(schema).ok());
    std::cout << "  created: ticks (ts SECOND, price FLOAT, volume INT)\n";
    std::cout << "  PASS\n\n";

    // === Test 2: Appender ===
    std::cout << "=== Test 2: Appender ===\n";
    auto app = conn.CreateAppender("ticks");
    assert(app.ok());
    for (int i = 0; i < 5; ++i) {
        auto sr = app->AppendRow(base_ts + i * 60'000'000LL, 100.5 + i * 0.3, 1000LL * (i + 1));
        if (!sr.ok()) std::cerr << "AppendRow error: " << sr.message() << "\n";
        assert(sr.ok());
    }
    assert(app->total_rows() == 5);
    auto sc = app->Close();
    if (!sc.ok()) std::cerr << "Close error: " << sc.message() << "\n";
    assert(sc.ok());
    std::cout << "  PASS\n\n";

    // === Test 3: Insert ===
    std::cout << "=== Test 3: Insert ===\n";
    auto s3 = conn.Insert("ticks", {base_ts + 300'000'000LL, 102.5, 2500LL});
    if (!s3.ok()) std::cerr << "Insert error: " << s3.message() << "\n";
    assert(s3.ok());
    std::cout << "  PASS\n\n";

    // === Test 4: Select * ===
    std::cout << "=== Test 4: Select * ===\n";
    auto r = conn.Select("ticks");
    assert(r.ok() && r->RowCount() == 6);
    PrintResult(*r);
    std::cout << "  PASS\n\n";

    // === Test 5: Select projection ===
    std::cout << "=== Test 5: Select projection ===\n";
    auto r2 = conn.Select("ticks", {"price", "volume"});
    assert(r2.ok() && r2->ColumnCount() == 2);
    PrintResult(*r2);
    std::cout << "  PASS\n\n";

    // === Test 6: 时间范围 ===
    std::cout << "=== Test 6: Time range ===\n";
    auto r_from = conn.Select("ticks", {"*"}, base_ts + 180'000'000LL);
    assert(r_from.ok() && r_from->RowCount() == 3);
    std::cout << "  from_ts:\n";
    PrintResult(*r_from);

    auto r_range = conn.Select("ticks", {"*"}, base_ts + 120'000'000LL, base_ts + 240'000'000LL);
    assert(r_range.ok() && r_range->RowCount() == 3);
    std::cout << "  range:\n";
    PrintResult(*r_range);
    std::cout << "  PASS\n\n";

    // === Test 7: 不同精度 ===
    std::cout << "=== Test 7: Different precisions ===\n";

    TableSchema schema2("sensors");
    schema2.AddColumn("ts", ColumnType::kTimestamp, TimePrecision::MICRO);
    schema2.AddColumn("val", ColumnType::kFloat);
    conn.CreateTable(schema2);

    auto app2 = conn.CreateAppender("sensors");
    assert(app2.ok());
    // 带有微秒精度的时间戳
    app2->AppendRow(base_ts + 123456LL, 23.5);
    app2->AppendRow(base_ts + 654321LL, 24.1);
    app2->Close();

    std::cout << "  MICRO precision:\n";
    auto rs = conn.Select("sensors");
    assert(rs.ok());
    PrintResult(*rs);

    // 同表不同精度
    TableSchema schema3("weather");
    schema3.AddColumn("day", ColumnType::kTimestamp, TimePrecision::DAY);
    schema3.AddColumn("hour", ColumnType::kTimestamp, TimePrecision::HOUR);
    schema3.AddColumn("minute", ColumnType::kTimestamp, TimePrecision::MINUTE);
    schema3.AddColumn("second", ColumnType::kTimestamp, TimePrecision::SECOND);
    schema3.AddColumn("milli", ColumnType::kTimestamp, TimePrecision::MILLI);
    schema3.AddColumn("micro", ColumnType::kTimestamp, TimePrecision::MICRO);
    conn.CreateTable(schema3);

    auto app3 = conn.CreateAppender("weather");
    assert(app3.ok());
    int64_t now = 1767264605123456LL;  // 带微秒
    app3->AppendRow(now, now, now, now, now, now);
    app3->Close();

    std::cout << "  All precisions (same ts, different formats):\n";
    auto rw = conn.Select("weather");
    assert(rw.ok());
    PrintResult(*rw);
    std::cout << "  PASS\n\n";

    // === Test 8: Error handling ===
    std::cout << "=== Test 8: Error handling ===\n";
    auto s = conn.Insert("nonexistent", {});
    assert(!s.ok() && s.code() == StatusCode::kNotFound);
    std::cout << "  not found: " << s.message() << "\n";

    auto r3 = conn.Select("nonexistent");
    assert(!r3.ok());
    std::cout << "  select missing: " << r3.status.message() << "\n";
    std::cout << "  PASS\n\n";
}

static void RunReadOnlyTests(const std::string& data_dir) {
    std::cout << "=== Test 9: OpenReadOnly ===\n";
    auto db_ro = WaveDB::Open(data_dir, {.read_only = true});
    assert(db_ro.ok() && db_ro->read_only());
    Connection conn(*db_ro);

    auto r = conn.Select("ticks");
    assert(r.ok() && r->RowCount() == 6);
    std::cout << "  read: " << r->RowCount() << " rows\n";

    auto s = conn.Insert("ticks", {base_ts, 1.0, int64_t(1)});
    assert(!s.ok());
    std::cout << "  insert rejected: " << s.message() << "\n";
    std::cout << "  PASS\n\n";
}

int main() {
    const std::string data_dir = "/tmp/wavedb_test";
    std::system(("rm -rf " + data_dir).c_str());

    RunWriteTests(data_dir);
    RunReadOnlyTests(data_dir);

    //    std::system(("rm -rf " + data_dir).c_str());
    std::cout << "=== All tests passed ===\n";
    return 0;
}
