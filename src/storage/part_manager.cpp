// PartManager 实现：Part 加载、排序、时间裁剪、合并。
//
// n_ Part 的生命周期由 Part 类管理（命名、创建、删除、merge offset）。
// m_ Part 的生命周期由 PartManager 管理（命名、创建、序号）。
//
// m_ 序号持久化在 <parts_dir>/.m_seq_<YYYYMMDD> 文件中，每天从 000001 开始。

#include "src/storage/part_manager.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// 解析 Part 目录名 n_YYYYMMDD_XXXXXX 或 m_YYYYMMDD_XXXXXX 的序列号。失败返回 -1。
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

// m_ 序号计数器：读 <parts_dir>/.m_seq 文件（格式 "YYYYMMDD last_seq"）。
// 同天自增，不同天从 1 开始。
static int NextMergeSeq(const std::string& parts_dir, int date) {
    std::string seq_path = parts_dir + "/.m_seq";
    int last_seq = 0;
    int last_date = 0;
    std::ifstream ifs(seq_path);
    if (ifs.is_open()) {
        ifs >> last_date >> last_seq;
        ifs.close();
    }
    int next_seq = (date == last_date) ? last_seq + 1 : 1;
    std::ofstream ofs(seq_path, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << date << " " << next_seq;
        ofs.close();
    }
    return next_seq;
}

// 构造 m_ Part 目录完整路径：<table_dir>/parts/m_<YYYYMMDD>_<XXXXXX>。
static std::string MakeMergePartDir(const std::string& table_dir, int date, int seq) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "m_%08d_%06d", date, seq);
    return table_dir + "/parts/" + buf;
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
            pm.parts_.push_back(std::move(*part));
        }
        // 损坏的 Part 静默跳过（与 Catalog 的处理策略一致）
    }
    ::closedir(dp);

    // 按 min_ts 排序（时间顺序），使 GetPartsInRange 可提前终止
    std::sort(pm.parts_.begin(), pm.parts_.end(), [](const Part& a, const Part& b) { return a.min_ts() < b.min_ts(); });

    return pm;
}

static bool IsMergedPart(const Part& p) {
    const auto& d = p.dir();
    auto slash = d.rfind('/');
    return slash != std::string::npos && slash + 1 < d.size() && d[slash + 1] == 'm';
}

std::vector<const Part*> PartManager::GetPartsInRange(Timestamp from_ts, Timestamp to_ts) const {
    std::vector<const Part*> result;
    for (const auto& part : parts_) {
        if (part.max_ts() < from_ts) continue;
        if (to_ts > 0 && part.min_ts() > to_ts) continue;
        result.push_back(&part);
    }
    return result;
}

std::vector<const Part*> PartManager::GetMergedPartsInRange(Timestamp from_ts, Timestamp to_ts) const {
    std::vector<const Part*> result;
    for (const auto& part : parts_) {
        if (!IsMergedPart(part)) continue;  // 只读 m_
        if (part.max_ts() < from_ts) continue;
        if (to_ts > 0 && part.min_ts() > to_ts) continue;
        result.push_back(&part);
    }
    return result;
}

std::vector<Part> PartManager::TakeMergedParts() {
    std::vector<Part> merged;
    for (auto& p : parts_) {
        if (IsMergedPart(p)) merged.push_back(std::move(p));
    }
    // 保留 n_ parts 在原 vector 中（测试/Merge 仍需要）
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(),
        [](const Part& p) { return IsMergedPart(p); }), parts_.end());
    return merged;
}

size_t PartManager::total_rows() const {
    size_t n = 0;
    for (const auto& p : parts_) n += p.row_count();
    return n;
}


// 将时间戳截断到合并策略对应的边界（仅分支 B 使用）。
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
        case MergePolicy::BY_WEEK: {
            // 周一零点
            int64_t sec = ts / 1'000'000;
            struct tm tm_buf;
            gmtime_r(&sec, &tm_buf);
            int days_to_monday = (tm_buf.tm_wday + 6) % 7;  // Mon=0, Sun=6
            tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
            return (static_cast<int64_t>(timegm(&tm_buf)) - days_to_monday * 86'400LL) * 1'000'000LL;
        }
        case MergePolicy::BY_MONTH: {
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
    if (parts_.empty()) return 0;

    // NONE 策略 + 无 MAX_ROWS → 1:1 merge（每个 n_ 直接变成 m_）
    if (cfg.policy == MergePolicy::NONE && cfg.merge_target_rows <= 0) {
        size_t ncols = parts_[0].schema().column_count();
        int ts_ci = -1;
        for (size_t ci = 0; ci < ncols; ++ci)
            if (parts_[0].schema().column_at(ci).type == ColumnType::TIMESTAMP) { ts_ci = static_cast<int>(ci); break; }

        std::vector<size_t> to_del;
        size_t merged_count = 0;
        for (size_t i = 0; i < parts_.size(); ++i) {
            if (IsMergedPart(parts_[i])) continue;
            std::vector<std::vector<Value>> cols(ncols);
            for (size_t ci = 0; ci < ncols; ++ci) {
                auto col = parts_[i].ReadColumn(static_cast<int>(ci), parts_[0].schema().column_at(ci).type);
                if (!col.ok()) return merged_count;
                cols[ci] = std::move(*col);
            }
            int64_t batch_min = 0, batch_max = 0;
            if (ts_ci >= 0 && !cols[ts_ci].empty()) {
                batch_min = std::get<int64_t>(cols[ts_ci].front());
                batch_max = std::get<int64_t>(cols[ts_ci].back());
            }
            int m_date = Part::TsToDate(batch_min);
            int m_seq = NextMergeSeq(table_dir_ + "/parts", m_date);
            std::string part_dir = MakeMergePartDir(table_dir_, m_date, m_seq);

            Result<Part> result = cfg.use_compression
                ? Part::CreateBlocked(part_dir, parts_[0].schema(), cols, batch_min, batch_max)
                : Part::CreateWithPath(part_dir, parts_[0].schema(), cols, batch_min, batch_max);

            if (!result.ok()) return merged_count;
            parts_.push_back(std::move(*result));
            to_del.push_back(i);
            ++merged_count;
        }
        std::sort(to_del.begin(), to_del.end(), std::greater<size_t>());
        for (size_t idx : to_del) { parts_[idx].Delete(); parts_.erase(parts_.begin() + static_cast<ptrdiff_t>(idx)); }
        return merged_count;
    }

    size_t ncols = parts_[0].schema().column_count();
    int ts_ci = -1;
    for (size_t ci = 0; ci < ncols; ++ci)
        if (parts_[0].schema().column_at(ci).type == ColumnType::TIMESTAMP) { ts_ci = static_cast<int>(ci); break; }

    if (cfg.merge_target_rows > 0) {
        // ---- 分支 A：渐进式 MAX_ROWS 合并 ----
        size_t m_target = static_cast<size_t>(cfg.merge_target_rows);
        size_t merged_count = 0;

        // 查找 in-progress m_（row_count < target 且标记 in_progress）
        Part* inprog_m = nullptr;
        for (auto& p : parts_) {
            if (IsMergedPart(p) && p.is_in_progress()) { inprog_m = &p; break; }
        }

        // 收集 n_ 数据（不读 m_，渐进追加）
        std::vector<std::vector<Value>> batch_cols(ncols);
        size_t remain = inprog_m ? (m_target - inprog_m->row_count()) : m_target;
        if (inprog_m && inprog_m->row_count() >= m_target) {
            inprog_m->set_in_progress(false);
            inprog_m->PersistMeta();
            return 0;
        }

        // 统计 n_ 总行数
        size_t total_n = 0;
        for (auto& p : parts_) { if (!IsMergedPart(p)) total_n += p.row_count(); }
        if (total_n == 0) return 0;

        // 从最小 n_ 往上累加
        std::vector<size_t> to_del;
        size_t partial_i = SIZE_MAX, partial_take = 0;

        for (size_t i = 0; i < parts_.size(); ++i) {
            if (remain == 0) break;
            auto& np = parts_[i];
            if (IsMergedPart(np)) continue;
            size_t avail = np.row_count();
            if (avail == 0) continue;
            size_t take = std::min(avail, remain);
            for (size_t ci = 0; ci < ncols; ++ci) {
                auto col = np.ReadColumnRange(static_cast<int>(ci), parts_[0].schema().column_at(ci).type, 0, take);
                if (!col.ok()) return merged_count;
                for (auto& v : *col) batch_cols[ci].push_back(std::move(v));
            }
            remain -= take;
            if (take == avail) to_del.push_back(i);
            else { partial_i = i; partial_take = take; }
        }

        if (batch_cols[0].empty() && !inprog_m) return 0;

        bool complete = inprog_m ? (inprog_m->row_count() + batch_cols[0].size() >= m_target) : (batch_cols[0].size() >= m_target);

        if (inprog_m) {
            if (cfg.use_compression)
                inprog_m->AppendColumnsBlocked(batch_cols);
            else
                inprog_m->AppendColumns(batch_cols);
            inprog_m->set_in_progress(!complete);
            inprog_m->PersistMeta();
        } else {
            int64_t batch_min = 0, batch_max = 0;
            if (ts_ci >= 0 && !batch_cols[ts_ci].empty()) {
                batch_min = std::get<int64_t>(batch_cols[ts_ci].front());
                batch_max = std::get<int64_t>(batch_cols[ts_ci].back());
            }
            int m_date = Part::TsToDate(batch_min);
            int m_seq = NextMergeSeq(table_dir_ + "/parts", m_date);
            std::string part_dir = MakeMergePartDir(table_dir_, m_date, m_seq);

            Result<Part> result = cfg.use_compression
                ? Part::CreateBlocked(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max)
                : Part::CreateWithPath(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);

            if (!result.ok()) return merged_count;
            Part new_m = std::move(*result);
            new_m.set_in_progress(!complete);
            new_m.PersistMeta();
            parts_.push_back(std::move(new_m));
        }

        if (partial_i != SIZE_MAX) parts_[partial_i].DiscardFirstRows(partial_take);

        std::sort(to_del.begin(), to_del.end(), std::greater<size_t>());
        for (size_t idx : to_del) {
            parts_[idx].Delete();
            parts_.erase(parts_.begin() + static_cast<ptrdiff_t>(idx));
        }

        return 1;

    } else {
        // ---- 分支 B：渐进式 m_（纯 policy）----
        for (auto& p : parts_) {
            if (IsMergedPart(p)) continue;
            p.set_merge_boundary(ComputeMergeBoundary(p.min_ts(), cfg.policy));
        }

        std::map<int64_t, std::vector<size_t>> n_groups;
        for (size_t i = 0; i < parts_.size(); ++i) {
            if (IsMergedPart(parts_[i])) continue;
            n_groups[parts_[i].merge_boundary()].push_back(i);
        }

        // 计算 boundary 对应的结束时间
        auto BoundaryEnd = [&cfg](int64_t boundary) -> int64_t {
            switch (cfg.policy) {
                case MergePolicy::BY_HOUR:  return boundary + 3'600'000'000LL - 1;
                case MergePolicy::BY_DAY:   return boundary + 86'400'000'000LL - 1;
                case MergePolicy::BY_WEEK:  return boundary + 7LL * 86'400'000'000LL - 1;
                case MergePolicy::BY_MONTH: {
                    int64_t sec = boundary / 1'000'000;
                    struct tm tm_buf; gmtime_r(&sec, &tm_buf);
                    tm_buf.tm_mon += 1;
                    if (tm_buf.tm_mon > 11) { tm_buf.tm_mon = 0; tm_buf.tm_year += 1; }
                    tm_buf.tm_mday = 1; tm_buf.tm_hour = 0; tm_buf.tm_min = 0; tm_buf.tm_sec = 0;
                    return static_cast<int64_t>(timegm(&tm_buf)) * 1'000'000LL - 1;
                }
                default: return INT64_MAX;
            }
        };

        // 查找 in-progress m_
        Part* inprog_m = nullptr;
        for (auto& p : parts_) {
            if (IsMergedPart(p) && p.is_in_progress()) { inprog_m = &p; break; }
        }

        // 确定当前 boundary
        int64_t cur_boundary;
        if (inprog_m)
            cur_boundary = inprog_m->merge_boundary();
        else
            cur_boundary = n_groups.begin()->first;

        // 收集同 boundary 的 n_ 数据
        std::vector<std::vector<Value>> batch_cols(ncols);
        std::vector<size_t> to_del;
        bool got_new_data = false;
        int64_t bed = BoundaryEnd(cur_boundary);
        auto it = n_groups.find(cur_boundary);
        if (it != n_groups.end()) {
            for (size_t idx : it->second) {
                size_t keep = parts_[idx].row_count();
                if (ts_ci >= 0 && parts_[idx].max_ts() > bed) {
                    auto ts_col = parts_[idx].ReadColumn(ts_ci, ColumnType::TIMESTAMP);
                    if (!ts_col.ok()) return 0;
                    keep = 0;
                    for (auto& v : *ts_col) { if (std::get<int64_t>(v) > bed) break; ++keep; }
                }
                if (keep == 0) continue;
                for (size_t ci = 0; ci < ncols; ++ci) {
                    auto col = parts_[idx].ReadColumnRange(static_cast<int>(ci), parts_[0].schema().column_at(ci).type, 0, keep);
                    if (!col.ok()) return 0;
                    for (auto& v : *col) batch_cols[ci].push_back(std::move(v));
                }
                if (keep == parts_[idx].row_count()) to_del.push_back(idx);
                else parts_[idx].DiscardFirstRows(keep);
                got_new_data = true;
            }
        }

        if (!got_new_data && inprog_m) {
            if (n_groups.size() >= 2 || (n_groups.size() == 1 && n_groups.begin()->first != cur_boundary)) {
                inprog_m->set_in_progress(false);
                inprog_m->PersistMeta();
            }
        } else if (got_new_data) {
            bool complete = n_groups.size() >= 2;

            if (inprog_m) {
                // 渐进追加：直接追加到已有 m_ 的 .col 文件
                if (cfg.use_compression)
                    inprog_m->AppendColumnsBlocked(batch_cols);
                else
                    inprog_m->AppendColumns(batch_cols);
                inprog_m->set_in_progress(!complete);
                inprog_m->PersistMeta();
            } else {
                int64_t batch_min = 0, batch_max = 0;
                if (ts_ci >= 0 && !batch_cols[ts_ci].empty()) {
                    batch_min = std::get<int64_t>(batch_cols[ts_ci].front());
                    batch_max = std::get<int64_t>(batch_cols[ts_ci].back());
                }
                int m_date = Part::TsToDate(batch_min);
                int m_seq = NextMergeSeq(table_dir_ + "/parts", m_date);
                std::string part_dir = MakeMergePartDir(table_dir_, m_date, m_seq);

                Result<Part> result = cfg.use_compression
                    ? Part::CreateBlocked(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max)
                    : Part::CreateWithPath(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);
                if (!result.ok()) return 0;
                Part new_m = std::move(*result);
                new_m.set_in_progress(!complete);
                new_m.set_merge_boundary(cur_boundary);
                new_m.PersistMeta();
                parts_.push_back(std::move(new_m));
            }
        } else {
            return 0;
        }

        std::sort(to_del.begin(), to_del.end(), std::greater<size_t>());
        for (size_t idx : to_del) {
            parts_[idx].Delete();
            parts_.erase(parts_.begin() + static_cast<ptrdiff_t>(idx));
        }
        return 1;
    }
}

}  // namespace wavedb
