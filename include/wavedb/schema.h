// 表 Schema 定义与 JSON 序列化。
//
// TableSchema 是表结构的运行时表示，同时负责 schema.json 的读写。
//
// 设计约束：
//   - schema.json 是人类可读的格式化 JSON，方便运维排查。
//   - 序列化格式是 WaveDB 文件格式的一部分，变更需版本化且向后兼容。
//   - 当前不支持 ALTER TABLE DROP COLUMN 等破坏性变更，
//     仅支持 ADD FIELD（新增列写在 schema.json 中，旧 Part 读取旧列数）。
//   - RowByteSize 假设所有列定长（8 字节），用于快速校准。
//     未来若加变长列，需要额外索引结构。
//
// 线程安全：TableSchema 只在建表时写入一次，后续只读。
// 多线程并发读取安全（不可变共享），写入由 Catalog 锁保护。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wavedb/status.h"
#include "wavedb/types.h"

namespace wavedb {

// 列定义：名称 + 类型 + 时间精度（仅对 TIMESTAMP 有效）。
struct ColumnDef {
    std::string name;
    ColumnType type;
    TimePrecision precision = TimePrecision::MICRO;  // 非 TIMESTAMP 列忽略此字段
};

// 表结构。管理列定义、合并配置、JSON 序列化/反序列化。
//
// 生命周期：
//   创建 → Catalog::CreateTable 写入 schema.json → 后续打开时从 JSON 反序列化。
//   运行时 schema 被视为不可变（当前不支持 ALTER TABLE 修改），
//   这简化了 Part 的读取逻辑——每个 Part 打开时使用相同的 schema 模板。
class TableSchema {
  public:
    TableSchema() = default;
    explicit TableSchema(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // ---- 合并配置 ----

    const MergeConfig& mergeConfig() const { return merge_config_; }
    MergeConfig& mergeConfig() { return merge_config_; }
    void setMergeConfig(MergeConfig cfg) { merge_config_ = cfg; }

    // ---- 列管理 ----

    // 添加列。列顺序即存储顺序，不可重排（保证文件格式稳定）。
    void AddColumn(std::string name, ColumnType type, TimePrecision prec = TimePrecision::MICRO) {
        columns_.push_back({std::move(name), type, prec});
    }

    // 按名称删除列。O(n) 线性扫描，删除后列索引重新排列。
    // 返回 false 表示列不存在。
    // 旧 Part 中仍保留被删列的 .col 文件——不重写历史数据，
    // 查询时该列不再出现在结果中。
    bool DropColumn(std::string_view name) {
        for (auto it = columns_.begin(); it != columns_.end(); ++it) {
            if (it->name == name) {
                columns_.erase(it);
                return true;
            }
        }
        return false;
    }

    size_t column_count() const { return columns_.size(); }
    const ColumnDef& column_at(size_t i) const { return columns_[i]; }
    const std::vector<ColumnDef>& columns() const { return columns_; }

    // 按名称查找列索引，O(n) 线性扫描（n 很小，通常 < 50 列）。
    // 返回 -1 表示未找到。
    int ColumnIndex(std::string_view name) const {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (columns_[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    const ColumnDef* FindColumn(std::string_view name) const {
        int idx = ColumnIndex(name);
        return idx >= 0 ? &columns_[idx] : nullptr;
    }

    // 行存储总字节数。当前所有列定长 8B，用于：
    //   (1) 快速校验 INSERT 行大小
    //   (2) 预留行式存储路径（未来可能用 mmap 的 row-wise scan）
    size_t RowByteSize() const {
        size_t n = 0;
        for (auto& col : columns_) n += ColumnTypeSize(col.type);
        return n;
    }

    // ---- JSON 序列化 ----
    //
    // 输出格式示例：
    //   {
    //     "name": "ticks",
    //     "columns": [
    //       {"name": "ts", "type": "TIMESTAMP", "precision": "SECOND"},
    //       {"name": "price", "type": "FLOAT"},
    //       {"name": "volume", "type": "INT"}
    //     ]
    //   }

    std::string ToJson() const;
    static Result<TableSchema> FromJson(std::string_view json);

  private:
    std::string name_;
    std::vector<ColumnDef> columns_;
    MergeConfig merge_config_{};
};

}  // namespace wavedb
