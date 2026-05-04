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
        // 每次查询重建 Connection，获取最新的 Catalog 快照（schema.json 变化可见）
        Connection conn(*db);
        auto r = conn.Select("kbars", {"*"}, 0, 0, 0);
        queries++;

        if (r.ok()) {
            uint64_t n = r->RowCount();
            size_t ncols = r->ColumnCount();

            // 列数变化
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

            // 行数变化
            if (n != last_rows) {
                std::cout << "#" << queries << " | rows: " << n;
                if (n > last_rows) std::cout << " (+" << (n - last_rows) << ")";
                std::cout << " | cols: " << ncols << "\n";

                // 打印最后 3 行
                if (n > 0) {
                    auto r_tail = conn.Select("kbars", {"*"}, 0, 0, 3);
                    if (r_tail.ok()) {
                        for (auto& row : r_tail->rows) {
                            std::cout << "  ";
                            for (size_t c = 0; c < ncols; ++c) {
                                if (c > 0) std::cout << " | ";
                                if (r_tail->column_types[c] == ColumnType::TIMESTAMP) {
                                    std::cout
                                        << FormatTimestamp(std::get<int64_t>(row[c]), r_tail->column_precisions[c]);
                                } else if (r_tail->column_types[c] == ColumnType::FLOAT) {
                                    char buf[32];
                                    std::snprintf(buf, sizeof(buf), "%.4f", std::get<double>(row[c]));
                                    std::cout << buf;
                                } else {
                                    std::cout << std::get<int64_t>(row[c]);
                                }
                            }
                            std::cout << "\n";
                        }
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
