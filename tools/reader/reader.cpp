// 读进程：持续读取，检查写入的数据
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
    Connection conn(*db);

    uint64_t last_rows = 0;
    uint64_t queries = 0;

    std::cout << "Reader started, polling every 100ms, Ctrl+C to stop\n";

    while (running) {
        // 查总数（limit=0，取全表行数）
        auto r = conn.Select("ticks", {"*"}, 0, 0, 0);
        queries++;

        if (r.ok()) {
            uint64_t n = r->RowCount();
            if (n != last_rows) {
                std::cout << "Query #" << queries << " | rows: " << n;
                if (n > last_rows) std::cout << " (+" << (n - last_rows) << ")";
                std::cout << "\n";

                // 用 limit=3 只看末尾
                if (n > 0) {
                    auto r_tail = conn.Select("ticks", {"*"}, 0, 0, 3);
                    if (r_tail.ok()) {
                        for (auto& row : r_tail->rows) {
                            std::cout << "  "
                                      << FormatTimestamp(std::get<int64_t>(row[0]), r_tail->column_precisions[0])
                                      << " | " << std::get<double>(row[1]) << " | " << std::get<int64_t>(row[2])
                                      << "\n";
                        }
                    }
                }
                last_rows = n;
            }
        } else {
            if (queries == 1) std::cout << "Waiting for table...\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nReader stopped. Total queries: " << queries << "\n";
    return 0;
}
