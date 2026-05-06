// 读进程：持续读取 kbars 表，监控行数/列数变化
// 用法: ./reader /tmp/wavedb_perf
// 退出: Ctrl+C (SIGINT)

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "wavedb.h"

using namespace wavedb;

static volatile bool running = true;

static void OnSignal(int) { running = false; }

// 打印一行 kbar 数据
static void PrintKBarRow(const QueryResult& r, size_t row_idx) {
    auto row = r.Row(row_idx);
    size_t ncols = r.ColumnCount();

    // TS
    std::cout << FormatTimestamp(int64_t(row["ts"]), r.column_precisions[0]);

    // OHLCV — 固定格式
    double open  = double(row["open"]);
    double high  = double(row["high"]);
    double low   = double(row["low"]);
    double close = double(row["close"]);
    int64_t vol  = int64_t(row["vol"]);

    char buf[128];
    std::snprintf(buf, sizeof(buf), " | O:%.4f H:%.4f L:%.4f C:%.4f V:%ld", open, high, low, close, vol);
    std::cout << buf;

    // ma5 / ma10（如果存在）
    if (ncols >= 8) {
        double ma5  = double(row["ma5"]);
        double ma10 = double(row["ma10"]);
        std::snprintf(buf, sizeof(buf), " | MA5:%.4f MA10:%.4f", ma5, ma10);
        std::cout << buf;
    }

    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <data_dir>\n";
        return 1;
    }
    std::string data_dir = argv[1];

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    auto db = WaveDB::Open(data_dir, {.read_only = true});
    if (!db.ok()) {
        std::cerr << "Open failed: " << db.status.message() << "\n";
        return 1;
    }

    uint64_t last_rows = 0;
    size_t last_cols = 0;
    uint64_t queries = 0;

    std::cout << "Reader started, polling every 100ms, Ctrl+C to stop\n";

    while (running) {
        Connection conn(*db);
        auto r = conn.Query("SELECT * FROM kbars");
        queries++;

        if (r.ok()) {
            uint64_t n = r->RowCount();
            size_t ncols = r->ColumnCount();

            if (ncols != last_cols) {
                std::cout << "\n*** Column count changed: " << last_cols << " → " << ncols << " ***\n";
                std::cout << "Columns: ";
                for (size_t c = 0; c < ncols; ++c) {
                    if (c > 0) std::cout << ", ";
                    std::cout << r->column_names[c];
                }
                std::cout << "\n\n";
                last_cols = ncols;
            }

            if (n != last_rows) {
                std::cout << "#" << queries << " | rows: " << n;
                if (n > last_rows) std::cout << " (+" << (n - last_rows) << ")";
                std::cout << " | cols: " << ncols << "\n";

                // 打印最后 3 行 kbar
                if (n > 0) {
                    size_t start = (n > 3) ? n - 3 : 0;
                    for (size_t i = start; i < n; ++i) {
                        std::cout << "  ";
                        PrintKBarRow(*r, i);
                    }
                }
                last_rows = n;
            }
        } else {
            if (queries == 1) std::cout << "Waiting for kbars table...\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nReader stopped. Total queries: " << queries << "\n";
    return 0;
}
