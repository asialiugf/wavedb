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

// 判断 parts_[i] 是否为 m_ Part（已合并产物，不参与再合并）。
static bool IsMergedPart(const Part& p) {
    const auto& d = p.dir();
    auto slash = d.rfind('/');
    return slash != std::string::npos && slash + 1 < d.size() && d[slash + 1] == 'm';
}

size_t PartManager::MergeParts(const MergeConfig& cfg) {
    if (cfg.policy == MergePolicy::NONE) return 0;
    if (parts_.empty()) return 0;

    size_t ncols = parts_[0].schema().column_count();
    int ts_ci = -1;
    for (size_t ci = 0; ci < ncols; ++ci)
        if (parts_[0].schema().column_at(ci).type == ColumnType::TIMESTAMP) { ts_ci = static_cast<int>(ci); break; }

    if (cfg.merge_target_rows > 0) {
        // ---- 分支 A：有 MAX_ROWS → 按 m_target_rows 减法合并 ----
        size_t m_target = static_cast<size_t>(cfg.merge_target_rows);
        size_t merged_count = 0;

        for (;;) {
            // 统计所有 n_ 剩余有效行总数
            size_t total_eff = 0;
            for (auto& p : parts_) {
                if (IsMergedPart(p)) continue;
                total_eff += p.row_count();
            }
            if (total_eff < m_target) break;  // 不够一个 m_，休眠等下次唤醒

            // 从最小 n_ 往上累加，remaining 减法
            size_t remaining = m_target;
            std::vector<size_t> to_del;
            std::vector<std::vector<Value>> batch_cols(ncols);
            size_t partial_i = SIZE_MAX;  // 部分消费的 n_ 索引
            size_t partial_take = 0;      // 它被取走的行数

            for (size_t i = 0; i < parts_.size(); ++i) {
                if (remaining == 0) break;
                auto& np = parts_[i];
                if (IsMergedPart(np)) continue;
                size_t avail = np.row_count();
                if (avail == 0) continue;

                size_t take = std::min(avail, remaining);
                for (size_t ci = 0; ci < ncols; ++ci) {
                    auto col = np.ReadColumnRange(static_cast<int>(ci), parts_[0].schema().column_at(ci).type, 0, take);
                    if (!col.ok()) return merged_count;
                    for (auto& v : *col) batch_cols[ci].push_back(std::move(v));
                }

                remaining -= take;

                if (take == avail) {
                    to_del.push_back(i);  // 完全消费 → 待删除
                } else {
                    partial_i = i;         // 部分消费，等 m_ 创建成功后再 DiscardFirstRows
                    partial_take = take;
                }
            }

            // 创建 m_ Part
            int64_t batch_min = 0, batch_max = 0;
            if (ts_ci >= 0 && !batch_cols[ts_ci].empty()) {
                batch_min = std::get<int64_t>(batch_cols[ts_ci].front());
                batch_max = std::get<int64_t>(batch_cols[ts_ci].back());
            }
            int m_date = Part::TsToDate(batch_min);
            int m_seq = NextMergeSeq(table_dir_ + "/parts", m_date);
            std::string part_dir = MakeMergePartDir(table_dir_, m_date, m_seq);
            auto result = Part::CreateWithPath(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);
            if (!result.ok()) return merged_count;
            parts_.push_back(std::move(*result));

            // m_ 创建成功后，处理部分消费的 n_（重写 .col 保留剩余行）
            if (partial_i != SIZE_MAX) {
                parts_[partial_i].DiscardFirstRows(partial_take);
            }

            // 删除完全消费的 n_
            std::sort(to_del.begin(), to_del.end(), std::greater<size_t>());
            for (size_t idx : to_del) {
                parts_[idx].Delete();
                parts_.erase(parts_.begin() + static_cast<ptrdiff_t>(idx));
            }

            ++merged_count;
        }
        return merged_count;

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

        // 收集数据：in-progress m_ 已有 + 同 boundary 的 n_
        std::vector<std::vector<Value>> batch_cols(ncols);
        if (inprog_m) {
            for (size_t ci = 0; ci < ncols; ++ci) {
                auto col = inprog_m->ReadColumn(static_cast<int>(ci), parts_[0].schema().column_at(ci).type);
                if (!col.ok()) return 0;
                batch_cols[ci] = std::move(*col);
            }
        }

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
            // in-progress m_ 没有新 n_ 可加 → 如果有其他 boundary 就关闭
            if (n_groups.size() >= 2 || (n_groups.size() == 1 && n_groups.begin()->first != cur_boundary)) {
                inprog_m->set_in_progress(false);
                inprog_m->PersistMeta();
            }
        } else if (got_new_data) {
            int64_t batch_min = 0, batch_max = 0;
            if (ts_ci >= 0 && !batch_cols[ts_ci].empty()) {
                batch_min = std::get<int64_t>(batch_cols[ts_ci].front());
                batch_max = std::get<int64_t>(batch_cols[ts_ci].back());
            }

            bool complete = n_groups.size() >= 2;

            if (inprog_m) {
                std::string m_dir = inprog_m->dir();
                std::error_code ec;
                std::filesystem::remove_all(m_dir, ec);
                auto result = Part::CreateWithPath(m_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);
                if (!result.ok()) return 0;
                Part new_m = std::move(*result);
                new_m.set_in_progress(!complete);
                new_m.set_merge_boundary(cur_boundary);
                new_m.PersistMeta();
                for (auto& p : parts_) {
                    if (IsMergedPart(p) && p.dir() == m_dir) { p = std::move(new_m); break; }
                }
            } else {
                int m_date = Part::TsToDate(batch_min);
                int m_seq = NextMergeSeq(table_dir_ + "/parts", m_date);
                std::string part_dir = MakeMergePartDir(table_dir_, m_date, m_seq);
                auto result = Part::CreateWithPath(part_dir, parts_[0].schema(), batch_cols, batch_min, batch_max);
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
