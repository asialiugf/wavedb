#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/storage/part.h"
#include "wavedb/status.h"

namespace wavedb {

// Part 管理器——单表的所有 Part 集合管理（n_ + m_ 均在此）。
//
// PartManager 负责：
//   (1) Open:  扫描 table_dir/parts/ 下的 Part 目录，按 min_ts 排序加载。
//               同时加载 n_YYYYMMDD_XXXXXX 和 m_YYYYMMDD_XXXXXX 格式的 Part。
//   (2) GetPartsInRange: 时间范围裁剪，返回与查询范围有交集的 Part 子集。
//   (3) MergeParts: 按合并策略合并 n_ Part 为 m_ Part。
//                   管理 m_ 的命名（m_YYYYMMDD_XXXXXX）和 m_ 序号。
//
// Part 排序：
//   Open 时按 min_ts 升序排列。Part 时间范围可能重叠（多个 Appender
//   可能写入重叠时间的数据），排序保证 GetPartsInRange 可以提前终止扫描。
//
// n_ 生命周期：由 Part 类管理（创建、删除、merge offset）。
// m_ 生命周期：由 PartManager 管理（MergeParts 创建、命名、序号），
//              m_ Part 读取仍通过 Part 类方法。
//
// 生命周期快照：
//   PartManager 在 Select 时临时构造，加载的是当前磁盘上的 Part 快照。
//   新写入的 Part 不会被已有 Select 看到（无 MVCC），这是正确的——
//   已打开的查询应看到一致性快照。

class PartManager {
  public:
    PartManager() = default;

    // 打开表目录，扫描并加载所有已有 Part（n_ 和 m_ 均加载）。
    static Result<PartManager> Open(std::string table_dir, const TableSchema& schema);

    // 获取与 [from_ts, to_ts] 有交集的 Part（min_ts ≤ to_ts && max_ts ≥ from_ts）。
    // to_ts=0 表示无上界。
    std::vector<const Part*> GetPartsInRange(Timestamp from_ts, Timestamp to_ts) const;

    const std::vector<Part>& all_parts() const { return parts_; }

    // 按合并策略合并 n_ Part 为 m_ Part。返回实际合并的组数。
    // 调用者须持有 LOCK_EX。m_ 命名由本方法管理。
    size_t MergeParts(const MergeConfig& cfg);

    // 转移 Part 所有权（供 QueryResult::Impl 接管 Part 生命周期）。
    std::vector<Part> TakeParts() { return std::move(parts_); }

    // 所有 Part 的总行数合计。
    size_t total_rows() const;

  private:
    std::string table_dir_;
    std::vector<Part> parts_;
};

}  // namespace wavedb
