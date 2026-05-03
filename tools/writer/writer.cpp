// 写进程：每秒写入 2000 笔 tick 数据
// 用法: ./writer /tmp/wavedb_perf
// 退出: Ctrl+C (SIGINT)

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
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

    // 打开数据库
    auto db = WaveDB::Open(data_dir);
    if (!db.ok()) {
        std::cerr << "Open failed: " << db.status.message() << "\n";
        return 1;
    }
    Connection conn(*db);

    // 建表（如果不存在）
    TableSchema schema("ticks");
    schema.AddColumn("ts", ColumnType::kTimestamp, TimePrecision::MICRO);
    schema.AddColumn("price", ColumnType::kFloat);
    schema.AddColumn("volume", ColumnType::kInt);

    if (!conn.CreateTable(schema).ok()) {
        // 表可能已存在，忽略
    }

    const int64_t batch_size = 2000;
    const int64_t step_us = 500;  // 2000笔/秒 = 每笔间隔500us

    int64_t next_ts =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    uint64_t total = 0;
    uint64_t batches = 0;

    auto app = conn.CreateAppender("ticks");
    if (!app.ok()) {
        std::cerr << "CreateAppender failed: " << app.status.message() << "\n";
        return 1;
    }

    std::cout << "Writer started, " << batch_size << " rows/sec, Ctrl+C to stop\n";

    while (running) {
        for (int64_t i = 0; i < batch_size; ++i) {
            double price = 100.0 + (double)(i % 100) * 0.01;
            int64_t volume = 100 + (i % 50);
            app->AppendRow(next_ts, price, volume);
            next_ts += step_us;
        }

        Status s = app->Flush();  // 写盘 + 清空缓冲，不重建 Appender
        if (!s.ok()) {
            std::cerr << "Flush failed: " << s.message() << "\n";
            continue;
        }

        total += batch_size;
        batches++;
        std::cout << "Flushed batch " << batches << " | total: " << total << " rows\n";

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    app->Close();
    std::cout << "\nWriter stopped. Total: " << total << " rows in " << batches << " batches\n";
    return 0;
}
