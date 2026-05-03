#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/storage/part.h"
#include "wavedb/status.h"

namespace wavedb {

// 管理一张表的所有 Part。
class PartManager {
  public:
    PartManager() = default;

    // 打开表目录，扫描 parts/ 下所有 Part。
    static Result<PartManager> Open(std::string table_dir, const TableSchema& schema);

    // 创建新 Part 目录并添加到管理器。返回 Part 引用。
    Result<Part*> CreatePart(
        int64_t min_ts,
        int64_t max_ts,
        const TableSchema& schema,
        const std::vector<std::vector<Value>>& columns);

    // 获取与时间范围有交集的 Part 列表。
    std::vector<const Part*> GetPartsInRange(Timestamp from_ts, Timestamp to_ts) const;

    const std::vector<Part>& all_parts() const { return parts_; }
    size_t total_rows() const;

  private:
    std::string NextPartDir() const;

    std::string table_dir_;
    std::vector<Part> parts_;
    int next_part_id_ = 1;
};

}  // namespace wavedb
