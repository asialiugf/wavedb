// PartManager 实现：Part 加载、排序、时间裁剪。

#include "src/storage/part_manager.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

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

// 从 "001" 这种三位数目录名解析 Part ID。
// 非纯数字返回 -1（跳过非 Part 目录如 .lock 等）。
static int ParsePartId(const std::string& name) {
    int id = 0;
    for (char c : name) {
        if (c < '0' || c > '9') return -1;
        id = id * 10 + (c - '0');
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

std::string PartManager::NextPartDir() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", next_part_id_);
    return table_dir_ + "/parts/" + buf;
}

}  // namespace wavedb
