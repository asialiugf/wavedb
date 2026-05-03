// 写进程：每秒写入 2000 笔 tick 数据
// 用法: ./writer /tmp/wavedb_perf
// 退出: Ctrl+C (SIGINT)

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <thread>

#include "src/common/types.h"
#include "src/engine/connection.h"
#include "src/engine/wavedb.h"

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
  int64_t ts_base =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  uint64_t total = 0;
  uint64_t batches = 0;

  std::cout << "Writer started, " << batch_size << " rows/sec, Ctrl+C to stop\n";

  while (running) {
    // 每秒一批：2000 行
    auto app = conn.CreateAppender("ticks");
    if (!app.ok()) {
      std::cerr << "CreateAppender failed: " << app.status.message() << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    for (int64_t i = 0; i < batch_size; ++i) {
      int64_t ts = ts_base + (batches * batch_size + i) * 500;  // 500us 间隔
      double price = 100.0 + (double)(i % 100) * 0.01;
      int64_t volume = 100 + (i % 50);
      app->AppendRow(ts, price, volume);
    }

    Status s = app->Close();
    if (!s.ok()) {
      std::cerr << "Close failed: " << s.message() << "\n";
      continue;
    }

    total += batch_size;
    batches++;
    std::cout << "Wrote batch " << batches << " | total: " << total
              << " rows\n";

    // 等 1 秒再写下一批
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << "\nWriter stopped. Total: " << total << " rows in " << batches
            << " batches\n";
  return 0;
}
