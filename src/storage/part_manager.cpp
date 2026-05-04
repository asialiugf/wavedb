// PartManager 实现：Part 加载、排序、时间裁剪。

#include "src/storage/part_manager.h"

#include <dirent.h>
#include <sys/stat.h>

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

// 解析 Part 目录名（n000001 或 m000001），返回 ID。
// 非 n/m 前缀返回 -1（跳过 .lock 等）。
static int ParsePartId(const std::string& name) {
    if (name.size() < 2) return -1;
    if (name[0] != 'n' && name[0] != 'm') return -1;
    int id = 0;
    for (size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        id = id * 10 + (name[i] - '0');
    }
    return id;
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
        int id = ParsePartId(part_name);
        if (id < 0) continue;  // 非三位数目录名 → 跳过

        std::string part_dir = parts_dir + "/" + part_name;
        if (!DirExists(part_dir)) continue;

        auto part = Part::Open(part_dir, schema);
        if (part.ok()) {
            pm.parts_.push_back(std::move(*part));
            // 记录最大 ID+1，确保新 Part 编号不冲突
            if (id >= pm.next_part_id_) pm.next_part_id_ = id + 1;
        }
        // 损坏的 Part 静默跳过（与 Catalog 的处理策略一致）
    }
    ::closedir(dp);

    // 按 min_ts 排序（时间顺序），使 GetPartsInRange 可提前终止
    std::sort(pm.parts_.begin(), pm.parts_.end(), [](const Part& a, const Part& b) { return a.min_ts() < b.min_ts(); });

    return pm;
}

Result<Part*> PartManager::CreatePart(
    int64_t min_ts,
    int64_t max_ts,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns) {
    std::string part_dir = NextPartDir();

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

static int ScanNextPartId(const std::string& parts_dir, char prefix) {
    int max_id = 0;
    DIR* dp = ::opendir(parts_dir.c_str());
    if (!dp) return 1;
    struct dirent* entry;
    while ((entry = ::readdir(dp)) != nullptr) {
        if (entry->d_name[0] != prefix) continue;
        int id = 0;
        bool ok = true;
        for (int i = 1; entry->d_name[i]; ++i) {
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') { ok = false; break; }
            id = id * 10 + (entry->d_name[i] - '0');
        }
        if (ok && id > max_id) max_id = id;
    }
    ::closedir(dp);
    return max_id + 1;
}

static std::string MakePartDir(const std::string& table_dir, char prefix, int id) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c%06d", prefix, id);
    return table_dir + "/parts/" + buf;
}

std::string PartManager::NextPartDir() const {
    return MakePartDir(table_dir_, 'n', ScanNextPartId(table_dir_ + "/parts", 'n'));
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

        // 统计总行数，不够 target 则跳过（等下一轮攒够）
        size_t total_rows = 0;
        for (size_t idx : n_indices) total_rows += parts_[idx].row_count();
        if (total_rows < target_rows && target_rows != SIZE_MAX) continue;

        // 读所有 n-Parts 数据
        size_t ncols = parts_[0].schema().column_count();
        std::vector<std::vector<Value>> all_cols(ncols);
        int64_t merged_min = INT64_MAX, merged_max = 0;

        for (size_t idx : n_indices) {
            auto& np = parts_[idx];
            for (size_t ci = 0; ci < ncols; ++ci) {
                auto col = np.ReadColumn(static_cast<int>(ci), parts_[0].schema().column_at(ci).type);
                if (!col.ok()) return merged_count;
                for (auto& v : *col) all_cols[ci].push_back(std::move(v));
            }
            if (np.min_ts() < merged_min) merged_min = np.min_ts();
            if (np.max_ts() > merged_max) merged_max = np.max_ts();
            to_delete.push_back(idx);
        }

        // 按 max_rows 拆分为多个 m-Part
        size_t batch_size = target_rows;
        size_t row_cursor = 0;

        while (row_cursor < total_rows) {
            size_t nrows = std::min(batch_size, total_rows - row_cursor);

            std::vector<std::vector<Value>> batch_cols(ncols);
            for (size_t ci = 0; ci < ncols; ++ci)
                batch_cols[ci].assign(all_cols[ci].begin() + row_cursor, all_cols[ci].begin() + row_cursor + nrows);

            std::string part_dir = MakePartDir(table_dir_, 'm', ScanNextPartId(table_dir_ + "/parts", 'm'));
            auto result = Part::Create(part_dir, parts_[0].schema(), batch_cols, merged_min, merged_max);
            if (!result.ok()) return merged_count;
            Part merged_part = std::move(*result);
            merged_part.set_merge_boundary(boundary);
            parts_.push_back(std::move(merged_part));
            row_cursor += nrows;
        }

        ++merged_count;
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
