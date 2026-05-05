// PartManager 实现：Part 加载、排序、时间裁剪。

#include "src/storage/part_manager.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>

namespace wavedb {

// ---- 工具 ----

static bool DirExists(std::string_view path) {
    struct stat st;
    return ::stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static Status EnsureDir(std::string_view path) {
    if (DirExists(path)) return Status::OK();
    if (::mkdir(std::string(path).c_str(), 0755) != 0)
        return Status(StatusCode::IO_ERROR, std::string("mkdir failed: ") + std::string(path));
    return Status::OK();
}

// 时间戳（微秒） → YYYYMMDD 整数
static int TsToDate(int64_t ts_us) {
    time_t sec = static_cast<time_t>(ts_us / 1'000'000);
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);
    return (tm_buf.tm_year + 1900) * 10000 + (tm_buf.tm_mon + 1) * 100 + tm_buf.tm_mday;
}

// 解析 Part 目录名 n_YYYYMMDD_XXXXXX 或 m_YYYYMMDD_XXXXXX，返回序列号。
static int ParsePartSeq(const std::string& name) {
    if (name.size() != 17) return -1;
    if ((name[0] != 'n' && name[0] != 'm') || name[1] != '_') return -1;
    for (int i = 2; i < 10; ++i) if (name[i] < '0' || name[i] > '9') return -1;
    if (name[10] != '_') return -1;
    int seq = 0;
    for (int i = 11; i < 17; ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        seq = seq * 10 + (name[i] - '0');
    }
    return seq;
}

// ---- PartManager ----

Result<PartManager> PartManager::Open(std::string table_dir, const TableSchema& schema) {
    PartManager pm;
    pm.table_dir_ = table_dir;

    std::string parts_dir = table_dir + "/parts";
    Status s = EnsureDir(parts_dir);
    if (!s.ok()) return s;

    DIR* dp = ::opendir(parts_dir.c_str());
    if (!dp) return Status(StatusCode::IO_ERROR, "cannot open parts dir");

    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string part_name = entry->d_name;
        std::string part_dir = parts_dir + "/" + part_name;
        if (!DirExists(part_dir)) continue;

        // 以 meta.json 存在为准判别合法 Part 目录，不依赖命名格式
        if (::access((part_dir + "/meta.json").c_str(), F_OK) != 0) continue;

        auto part = Part::Open(part_dir, schema);
        if (part.ok()) {
            // 尝试解析序列号，能解析则用于跟踪最大 ID
            int id = ParsePartSeq(part_name);
            if (id >= 0 && id >= pm.next_part_id_) pm.next_part_id_ = id + 1;
            pm.parts_.push_back(std::move(*part));
        }
        // 损坏的 Part 静默跳过（与 Catalog 的处理策略一致）
    }
    ::closedir(dp);

    // 按 min_ts 排序（时间顺序），使 GetPartsInRange 可提前终止
    std::sort(pm.parts_.begin(), pm.parts_.end(), [](const Part& a, const Part& b) { return a.min_ts() < b.min_ts(); });

    return pm;
}

// 持久化计数器：<parts_dir>/.next_<prefix>_seq 读取+递增，永远单调不复用。
static int NextSeq(const std::string& parts_dir, char prefix) {
    int max_seq = 0;
    DIR* dp = ::opendir(parts_dir.c_str());
    if (!dp) return 1;
    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] != prefix || entry->d_name[1] != '_') continue;
        bool ok = true;
        for (int i = 2; i < 10; ++i) if (entry->d_name[i] < '0' || entry->d_name[i] > '9') { ok = false; break; }
        if (!ok || entry->d_name[10] != '_') continue;
        int seq = 0;
        for (int i = 11; i < 17; ++i) {
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') { ok = false; break; }
            seq = seq * 10 + (entry->d_name[i] - '0');
        }
        if (ok && seq > max_seq) max_seq = seq;
    }
    ::closedir(dp);
    return max_seq + 1;
}

static std::string MakePartDir(const std::string& table_dir, char prefix, int date, int seq) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%c_%08d_%06d", prefix, date, seq);
    return table_dir + "/parts/" + buf;
}

Result<Part*> PartManager::CreatePart(
    int64_t min_ts,
    int64_t max_ts,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns) {
    int date = TsToDate(min_ts);
    int seq = NextSeq(table_dir_ + "/parts", 'n');
    std::string part_dir = MakePartDir(table_dir_, 'n', date, seq);

    auto part = Part::Create(part_dir, schema, columns, min_ts, max_ts);
    if (!part.ok()) return part.status;

    parts_.push_back(std::move(*part));
    return &parts_.back();  // 返回稳定引用（vector 元素地址）
}

std::vector<const Part*> PartManager::GetPartsInRange(Timestamp from_ts, Timestamp to_ts) const {
    std::vector<const Part*> result;
    // Part 按 min_ts 排序，因此可线性扫描：
    for (const auto& part : parts_) {
        // Part 的 [min_ts, max_ts] 与查询范围 [from_ts, to_ts] 无交集则跳过
        if (part.max_ts() < from_ts) continue;
        if (to_ts > 0 && part.min_ts() > to_ts) continue;
        result.push_back(&part);
    }
    return result;
}

size_t PartManager::total_rows() const {
    size_t n = 0;
    for (const auto& p : parts_) n += p.row_count();
    return n;
}

std::string PartManager::NextPartDir() const {
    return MakePartDir(table_dir_, 'n', 0, NextSeq(table_dir_ + "/parts", 'n'));
}

// 辅助：将时间戳截断到合并策略对应的边界。
static int64_t ComputeMergeBoundary(Timestamp ts, MergePolicy policy) {
    switch (policy) {
        case MergePolicy::BY_HOUR: {
            constexpr int64_t kHour = 3'600'000'000LL;
            return (ts / kHour) * kHour;
        }
        case MergePolicy::BY_DAY: {
            constexpr int64_t kDay = 86'400'000'000LL;
            return (ts / kDay) * kDay;
        }
        case MergePolicy::BY_MONTH: {
            // 取当月第一天的零点
            int64_t sec = ts / 1'000'000;
            struct tm tm_buf;
            gmtime_r(&sec, &tm_buf);
            tm_buf.tm_hour = 0;
            tm_buf.tm_min = 0;
            tm_buf.tm_sec = 0;
            tm_buf.tm_mday = 1;
            return static_cast<int64_t>(timegm(&tm_buf)) * 1'000'000LL;
        }
        default:
            return 0;
    }
}

size_t PartManager::MergeParts(const MergeConfig& cfg) {
    if (cfg.policy == MergePolicy::NONE) return 0;
    if (parts_.empty()) return 0;

    // 步骤 1：为每个 Part 计算 merge_boundary
    for (auto& p : parts_) {
        if (p.merge_boundary() == 0) p.set_merge_boundary(ComputeMergeBoundary(p.min_ts(), cfg.policy));
    }

    // 步骤 2：只分组 n-Parts（m-Parts 已完成，不参与合并）
    std::map<int64_t, std::vector<size_t>> n_groups;
    for (size_t i = 0; i < parts_.size(); ++i) {
        const auto& d = parts_[i].dir();
        // 提取目录名的第一个字符判断 n（normal）或 m（merged）
        auto slash = d.rfind('/');
        bool is_merged = (slash != std::string::npos && slash + 1 < d.size() && d[slash + 1] == 'm');
        if (!is_merged) {
            n_groups[parts_[i].merge_boundary()].push_back(i);
        }
    }

    // 步骤 3：合并 n-Parts，只有当行数达到 max_rows（或无限）才产生 m-Part
    size_t merged_count = 0;
    std::vector<size_t> to_delete;
    size_t target_rows = (cfg.max_rows_per_part > 0) ? static_cast<size_t>(cfg.max_rows_per_part) : SIZE_MAX;

    for (auto& [boundary, n_indices] : n_groups) {
        if (n_indices.empty()) continue;

        // 统计有效行数（跳过已 merge 的行），不够 target 则等下一轮攒够
        size_t total_eff = 0;
        for (size_t idx : n_indices) total_eff += parts_[idx].effective_row_count();
        if (total_eff < target_rows && target_rows != SIZE_MAX) continue;

        size_t ncols = parts_[0].schema().column_count();
        int ts_ci = -1;
        for (size_t ci = 0; ci < ncols; ++ci)
            if (parts_[0].schema().column_at(ci).type == ColumnType::TIMESTAMP) { ts_ci = static_cast<int>(ci); break; }

        size_t batch_size = target_rows;

        // 跟踪本轮 while 已完全消费的 Part（在 to_delete 前临时跳过）
        std::vector<bool> consumed(parts_.size(), false);

        while (true) {
            size_t group_eff = 0;
            for (size_t idx : n_indices)
                if (!consumed[idx]) group_eff += parts_[idx].effective_row_count();
            if (batch_size != SIZE_MAX && group_eff < batch_size) break;

            // 逐 Part 收集一个 batch 的数据
            std::vector<std::vector<Value>> batch_cols(ncols);
            size_t batch_got = 0;
            size_t partial_idx = SIZE_MAX;
            size_t partial_rows = 0;

            for (size_t idx : n_indices) {
                if (batch_got >= batch_size && batch_size != SIZE_MAX) break;
                if (consumed[idx]) continue;
                auto& np = parts_[idx];
                size_t eff = np.effective_row_count();
                if (eff == 0) { consumed[idx] = true; continue; }
                size_t take = std::min(eff, batch_size - batch_got);

                for (size_t ci = 0; ci < ncols; ++ci) {
                    auto col = np.ReadColumnRange(static_cast<int>(ci), parts_[0].schema().column_at(ci).type, 0, take);
                    if (!col.ok()) return merged_count;
                    for (auto& v : *col) batch_cols[ci].push_back(std::move(v));
                }

                batch_got += take;
                if (take == eff) {
                    to_delete.push_back(idx);
                    consumed[idx] = true;
                } else {
                    partial_idx = idx;
                    partial_rows = take;
                    break;
                }
            }

            if (batch_got == 0) break;  // 所有 Part 都已消费完

            // 创建 m-Part
            int64_t batch_min = 0, batch_max = 0;
            if (ts_ci >= 0 && !batch_cols[ts_ci].empty()) {
                batch_min = std::get<int64_t>(batch_cols[ts_ci].front());
                batch_max = std::get<int64_t>(batch_cols[ts_ci].back());
            }
            int m_date = TsToDate(batch_min);
            int m_seq = NextSeq(table_dir_ + "/parts", 'm');
            std::string part_dir = MakePartDir(table_dir_, 'm', m_date, m_seq);
            auto result = Part::Create(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);
            if (!result.ok()) return merged_count;
            Part merged_part = std::move(*result);
            merged_part.set_merge_boundary(boundary);
            parts_.push_back(std::move(merged_part));

            // 裁剪部分消耗的 n-Part
            if (partial_idx != SIZE_MAX) {
                parts_[partial_idx].ConsumeRows(partial_rows);
            }

            ++merged_count;
        }
    }

    // 步骤 4：删除已消费的 n-Part 目录
    std::sort(to_delete.begin(), to_delete.end(), std::greater<size_t>());
    for (size_t idx : to_delete) {
        std::error_code ec;
        std::filesystem::remove_all(parts_[idx].dir(), ec);
        parts_.erase(parts_.begin() + static_cast<ptrdiff_t>(idx));
    }

    return merged_count;
}

}  // namespace wavedb
