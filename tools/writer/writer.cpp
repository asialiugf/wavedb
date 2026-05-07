// 写进程：写入 KBar 数据 → ALTER TABLE ADD ma5/ma10 → UPDATE → 继续写
// 用法: ./writer /tmp/wavedb_perf
// 退出: Ctrl+C (SIGINT)

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iostream>
#include <thread>

#include "wavedb.h"

using namespace wavedb;

static volatile bool running = true;

static void OnSignal(int) { running = false; }

// 写 1000 行 KBar + ADD COLUMN + UPDATE 后继续写的阈值
static const int64_t kAlterThreshold = 1000;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <data_dir>\n";
        return 1;
    }
    std::string data_dir = argv[1];

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    auto db = WaveDB::Open(data_dir);
    if (!db.ok()) {
        std::cerr << "Open failed: " << db.status.message() << "\n";
        return 1;
    }

    // 建 kbars 表（6 列），MERGE BY HOUR MAX_ROWS 3500
    // 若表已存在且无 merge，自动添加 merge 配置；已有 merge 则保留原有
    {
        Connection conn(*db);
        auto ct = conn.Query(
            "CREATE TABLE kbars ("
            "  ts TIMESTAMP(MICRO),"
            "  open FLOAT,"
            "  high FLOAT,"
            "  low FLOAT,"
            "  close FLOAT,"
            "  vol INT"
            ") MERGE BY WEEK");
        if (!ct.ok() && ct.status.code() != StatusCode::ALREADY_EXISTS) {
            std::cerr << "CreateTable failed: " << ct.status.message() << "\n";
        }
    }

    const int64_t batch_size = 2000; // 每批写入 2000 行，控制在 1 秒内完成（含 ALTER + UPDATE），保持每秒 ~2000 行的速率
    const int64_t step_us = 1'000'000;  // 每行时间戳间隔 1 秒（100万微秒），确保每小时最多 3600 行，触发 MERGE

    // int64_t next_ts =
    //     std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
    //         .count();

    int64_t next_ts = 1778336456000000LL - 864000000000LL; // 从 2025-01-06 00:00:00 开始写，方便测试 MERGE（每小时 3600 行，24 小时 86400 行）

    uint64_t total = 0;
    uint64_t batches = 0;

    // 每次循环重建 Connection + Appender，确保 schema 最新
    Connection conn(*db);
    auto app = conn.CreateAppender("kbars");
    if (!app.ok()) {
        std::cerr << "CreateAppender failed: " << app.status.message() << "\n";
        return 1;
    }

    // 检查是否已有 ma5/ma10 列（之前跑过 ALTER TABLE）
    size_t ncols = conn.GetTableSchema("kbars")->column_count();
    bool has_ma = (ncols == 8);
    if (has_ma) std::cout << "Columns already have ma5,ma10 (previous ALTER preserved)\n";

    // 存储 close 值用于后续 MA 计算
    std::deque<double> close_history;

    std::cout << "Writer started (" << batch_size << " rows/sec), table: kbars (ts,open,high,low,close,vol";
    if (has_ma) std::cout << ",ma5,ma10";
    std::cout << ")\nAfter " << kAlterThreshold << " rows: ALTER TABLE ADD ma5,ma10 → UPDATE → continue\n";

    int iteration = 0;
    while (running) {
        for (int64_t i = 0; i < batch_size; ++i) {

            iteration ++ ;
            double open = 10000.0 + iteration * 0.01;
            double high = open ;
            double low = open;
            double close = open ;
            int64_t vol = iteration ;

            close_history.push_back(close);

            if (has_ma) {
                double ma5 = 0.0, ma10 = 0.0;
                if (close_history.size() >= 5) {
                    double sum = 0;
                    for (size_t j = close_history.size() - 5; j < close_history.size(); ++j) sum += close_history[j];
                    ma5 = sum / 5.0;
                }
                if (close_history.size() >= 10) {
                    double sum = 0;
                    for (size_t j = close_history.size() - 10; j < close_history.size(); ++j) sum += close_history[j];
                    ma10 = sum / 10.0;
                }
                app->AppendRow(next_ts, open, high, low, close, vol, ma5, ma10);
            } else {
                app->AppendRow(next_ts, open, high, low, close, vol);
            }
            // iteration ++ ;
            next_ts = next_ts + step_us + 1; 
        }

        Status s = app->Flush();
        if (!s.ok()) {
            std::cerr << "Flush failed: " << s.message() << "\n";
            continue;
        }

        total += batch_size;
        batches++;
        std::cout << "Flushed batch " << batches << " | total: " << total << " rows"
                  << (has_ma ? " (with ma5,ma10)" : "") << "\n";

        // 写满 1000 行后执行 ALTER TABLE
        if (!has_ma && total >= static_cast<uint64_t>(kAlterThreshold)) {
            std::cout << "\n=== " << total << " rows reached → ALTER TABLE ADD ma5, ma10 ===\n";

            // ADD COLUMN（若列已存在则跳过）
            Status sa = conn.AddColumn("kbars", "ma5", ColumnType::FLOAT);
            if (sa.ok()) {
                std::cout << "ma5 added\n";
            } else if (sa.code() == StatusCode::ALREADY_EXISTS) {
                std::cout << "ma5 already exists, skipping\n";
            } else {
                std::cerr << "Add ma5 failed: " << sa.message() << "\n";
            }
            Status sb = conn.AddColumn("kbars", "ma10", ColumnType::FLOAT);
            if (sb.ok()) {
                std::cout << "ma10 added\n";
            } else if (sb.code() == StatusCode::ALREADY_EXISTS) {
                std::cout << "ma10 already exists, skipping\n";
            } else {
                std::cerr << "Add ma10 failed: " << sb.message() << "\n";
            }

            // 读全表 close 列，算 MA5/MA10
            {
                // 用新的 Connection 确保 schema 最新
                Connection conn2(*db);
                auto r = conn2.Select("kbars", {"ts", "close"});
                if (r.ok()) {
                    size_t n = r->RowCount();
                    std::vector<Value> ma5_vals, ma10_vals;
                    ma5_vals.reserve(n);
                    ma10_vals.reserve(n);

                    std::deque<double> win5, win10;
                    for (size_t i = 0; i < n; ++i) {
                        double c = std::get<double>(r->rows[i][1]);
                        win5.push_back(c);
                        if (win5.size() > 5) win5.pop_front();
                        win10.push_back(c);
                        if (win10.size() > 10) win10.pop_front();

                        double m5 = 0.0, m10 = 0.0;
                        if (win5.size() == 5) {
                            double s5 = 0;
                            for (double v : win5) s5 += v;
                            m5 = s5 / 5.0;
                        }
                        if (win10.size() == 10) {
                            double s10 = 0;
                            for (double v : win10) s10 += v;
                            m10 = s10 / 10.0;
                        }
                        ma5_vals.push_back(m5);
                        ma10_vals.push_back(m10);
                    }

                    Status u5 = conn2.UpdateColumn("kbars", "ma5", r->Row("first")["ts"], r->Row("last")["ts"], ma5_vals);
                    std::cout << "UPDATE ma5(range): " << (u5.ok() ? "ok" : u5.message()) << " (" << ma5_vals.size()
                              << " rows)\n";

                    Status u10 = conn2.UpdateColumn("kbars", "ma10", ma10_vals);
                    std::cout << "UPDATE ma10(auto): " << (u10.ok() ? "ok" : u10.message()) << " (" << ma10_vals.size()
                              << " rows)\n";
                }
            }

            has_ma = true;
            std::cout << "=== ALTER complete, continuing writes with ma5,ma10 ===\n\n";

            // 重建 Appender：旧的用 6 列缓冲，Close 后从同一 Connection 重建即可获得 8 列 schema
            // AddColumn 已原地更新 Catalog 中的 TableSchema
            app->Close();
            app = conn.CreateAppender("kbars");
            if (!app.ok()) {
                std::cerr << "Recreate Appender failed: " << app.status.message() << "\n";
                break;
            }
        }

        // 控制写入速率
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    app->Close();
    std::cout << "\nWriter stopped. Total: " << total << " rows\n";
    return 0;
}
