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
    Connection conn(*db);

    // 建 kbars 表（6 列）
    TableSchema schema("kbars");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::MICRO);
    schema.AddColumn("open", ColumnType::FLOAT);
    schema.AddColumn("high", ColumnType::FLOAT);
    schema.AddColumn("low", ColumnType::FLOAT);
    schema.AddColumn("close", ColumnType::FLOAT);
    schema.AddColumn("vol", ColumnType::INT);

    if (!conn.CreateTable(schema).ok()) {
        // 表可能已存在
    }

    const int64_t batch_size = 200;
    const int64_t step_us = 5000;  // 200 行/秒 = 每笔间隔 5ms

    int64_t next_ts =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    uint64_t total = 0;
    uint64_t batches = 0;
    bool altered = false;

    auto app = conn.CreateAppender("kbars");
    if (!app.ok()) {
        std::cerr << "CreateAppender failed: " << app.status.message() << "\n";
        return 1;
    }

    // 存储 close 值用于后续 MA 计算
    std::deque<double> close_history;

    std::cout << "Writer started (" << batch_size << " rows/sec), table: kbars (ts,open,high,low,close,vol)\n";
    std::cout << "After " << kAlterThreshold << " rows: ALTER TABLE ADD ma5,ma10 → UPDATE → continue\n";

    while (running) {
        for (int64_t i = 0; i < batch_size; ++i) {
            double base = 100.0 + (total + i) * 0.01;
            double open = base;
            double high = base + 0.5;
            double low = base - 0.3;
            double close = base + 0.1;
            int64_t vol = 500 + (total + i) % 100;

            close_history.push_back(close);

            if (altered) {
                // 8 列：ts, open, high, low, close, vol, ma5, ma10
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
                // 6 列：ts, open, high, low, close, vol
                app->AppendRow(next_ts, open, high, low, close, vol);
            }
            next_ts += step_us;
        }

        Status s = app->Flush();
        if (!s.ok()) {
            std::cerr << "Flush failed: " << s.message() << "\n";
            continue;
        }

        total += batch_size;
        batches++;
        std::cout << "Flushed batch " << batches << " | total: " << total << " rows"
                  << (altered ? " (with ma5,ma10)" : "") << "\n";

        // 写满 1000 行后执行 ALTER TABLE
        if (!altered && total >= static_cast<uint64_t>(kAlterThreshold)) {
            std::cout << "\n=== " << total << " rows reached → ALTER TABLE ADD ma5, ma10 ===\n";

            // ADD COLUMN
            Status sa = conn.AddColumn("kbars", "ma5", ColumnType::FLOAT);
            if (!sa.ok()) {
                std::cerr << "Add ma5 failed: " << sa.message() << "\n";
            }
            Status sb = conn.AddColumn("kbars", "ma10", ColumnType::FLOAT);
            if (!sb.ok()) {
                std::cerr << "Add ma10 failed: " << sb.message() << "\n";
            }

            // 读全表 close 列，算 MA5/MA10
            auto r = conn.Select("kbars", {"ts", "close"});
            if (r.ok()) {
                size_t n = r->RowCount();
                std::vector<Value> ma5_vals, ma10_vals;
                ma5_vals.reserve(n);
                ma10_vals.reserve(n);

                std::deque<double> win5, win10;
                for (size_t i = 0; i < n; ++i) {
                    double c = std::get<double>(r->rows[i][1]);  // close
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

                // 方式一：指定 ts 范围（用 RowView 取首尾 ts）
                // Timestamp t0 = std::get<int64_t>(r->Row("first")["ts"]);
                // Timestamp t2 = std::get<int64_t>(r->Row(0)["ts"]);
                // assert(t0 == t2);  // 确保首行 ts 与 Row(0) ts 一致
                // Timestamp t1 = std::get<int64_t>(r->Row("last")["ts"]);
                // Status u5 = conn.UpdateColumn("kbars", "ma5", t0, t1, ma5_vals);

                // 方式一：指定 ts 范围（Cell 隐式转 Timestamp，直接当参数）
                Status u5 = conn.UpdateColumn("kbars", "ma5", r->Row("first")["ts"], r->Row("last")["ts"], ma5_vals);
                std::cout << "UPDATE ma5(range): " << (u5.ok() ? "ok" : u5.message()) << " (" << ma5_vals.size()
                          << " rows)\n";

                // 方式二：自动全量更新
                Status u10 = conn.UpdateColumn("kbars", "ma10", ma10_vals);
                std::cout << "UPDATE ma10(auto): " << (u10.ok() ? "ok" : u10.message()) << " (" << ma10_vals.size()
                          << " rows)\n";
            }

            altered = true;
            std::cout << "=== ALTER complete, continuing writes with ma5,ma10 ===\n\n";

            // 重建 Appender——schema 列数从 6 变为 8，缓冲区需重新分配
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
