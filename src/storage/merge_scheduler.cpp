// MergeScheduler 实现：后台线程 + 定时扫描 + 合并触发。
//
// Run() 循环每 interval_secs_ 秒扫描一次，或被 Notify() 提前唤醒。
// 对每个有写入的表：读取 schema.json → 检查 MergePolicy → 打开 PartManager
// → 获取 LOCK_EX → MergeParts() → 释放锁。

#include "src/storage/merge_scheduler.h"

#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include "src/storage/part_manager.h"
#include "wavedb/database.h"
#include "wavedb/schema.h"

namespace wavedb {

// 辅助：从文件读取 schema.json 内容
static std::string ReadFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string content(sz, '\0');
    if (sz > 0) { size_t n = std::fread(content.data(), 1, sz, f); (void)n; }
    std::fclose(f);
    return content;
}

MergeScheduler::MergeScheduler(std::string data_dir) : data_dir_(std::move(data_dir)) {
    thread_ = std::thread(&MergeScheduler::Run, this);
}

MergeScheduler::~MergeScheduler() { Shutdown(); }

void MergeScheduler::Notify(std::string_view table_name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_tables_.emplace(table_name);
    }
    cv_.notify_one();
}

void MergeScheduler::Shutdown() {
    running_.store(false);
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void MergeScheduler::Run() {
    while (running_.load()) {
        // 有 dirty 表时立即扫描，否则定时等待
        {
            std::unique_lock<std::mutex> lock(mutex_);
            bool has_dirty = !dirty_tables_.empty();
            if (!has_dirty)
                cv_.wait_for(lock, std::chrono::seconds(interval_secs_));
        }

        if (!running_.load()) break;

        // 扫描 data_dir 下所有表目录
        DIR* dp = ::opendir(data_dir_.c_str());
        if (!dp) continue;
        struct dirent* entry;
        while ((entry = ::readdir(dp)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string table_dir = data_dir_ + "/" + entry->d_name;
            struct stat st;
            if (::stat(table_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            std::string schema_path = table_dir + "/schema.json";
            std::string json = ReadFile(schema_path);
            if (json.empty()) continue;

            auto schema_result = TableSchema::FromJson(json);
            if (!schema_result.ok()) continue;

            const auto& schema = *schema_result;
            const auto& cfg = schema.mergeConfig();
            if (cfg.policy == MergePolicy::NONE) continue;

            auto pm = PartManager::Open(table_dir, schema);
            if (!pm.ok()) continue;

            size_t merged = pm->MergeParts(cfg);
            if (merged > 0) {
                std::lock_guard<std::mutex> lk(mutex_);
                dirty_tables_.emplace(entry->d_name);
            }
        }
        ::closedir(dp);

        // 扫描完成后清理 dirty 标记，下一轮若又有 WritePart 会重新 Notify
        {
            std::lock_guard<std::mutex> lk(mutex_);
            dirty_tables_.clear();
        }
    }
}

}  // namespace wavedb
