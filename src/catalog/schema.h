#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "src/common/status.h"
#include "src/common/types.h"

namespace wavedb {

struct ColumnDef {
    std::string name;
    ColumnType type;
    TimePrecision precision = TimePrecision::MICRO;  // 仅对 TIMESTAMP 有效
};

class TableSchema {
  public:
    TableSchema() = default;
    explicit TableSchema(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // ---- 列管理 ----

    void AddColumn(std::string name, ColumnType type, TimePrecision prec = TimePrecision::MICRO) {
        columns_.push_back({std::move(name), type, prec});
    }

    size_t column_count() const { return columns_.size(); }
    const ColumnDef& column_at(size_t i) const { return columns_[i]; }
    const std::vector<ColumnDef>& columns() const { return columns_; }

    // 按名称查找列索引，返回 -1 表示未找到
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

    // 整行定长存储字节数
    size_t RowByteSize() const {
        size_t n = 0;
        for (auto& col : columns_) n += ColumnTypeSize(col.type);
        return n;
    }

    // ---- JSON 序列化 ----

    std::string ToJson() const;
    static Result<TableSchema> FromJson(std::string_view json);

  private:
    std::string name_;
    std::vector<ColumnDef> columns_;
};

}  // namespace wavedb
