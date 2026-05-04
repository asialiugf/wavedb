// 后台合并调度器。
//
// MergeScheduler 持有单一线程，定时扫描 data_dir 下所有表的 parts/ 目录，
// 对设置了 MergePolicy 的表自动调用 PartManager::MergeParts()。
//
// 线程模型：
//   - 一个后台线程，通过 condition_variable 定时唤醒或收到通知唤醒。
//   - 合并时获取 LOCK_EX（与 Writer 的 WritePart 相同），Reader 不受影响。
//   - Shutdown() 设置停止标志 + 通知 + join，析构时自动调用。
//
// 生命周期：
//   由 WaveDB 创建和持有。WaveDB::Open() 时启动，~WaveDB() 时自动停止。
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace wavedb {

class MergeScheduler {
  public:
    explicit MergeScheduler(std::string data_dir);
    ~MergeScheduler();

    // 通知有新数据写入（非阻塞），唤醒后台线程提前扫描。
    void Notify(std::string_view table_name);

    // 停止后台线程并等待退出。
    void Shutdown();

  private:
    void Run();

    std::string data_dir_;
    std::atomic<bool> running_{true};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<std::string> dirty_tables_;
    std::thread thread_;
    int interval_secs_ = 5;
};

}  // namespace wavedb
