// 目录管理器——表的注册与发现。
//
// Catalog 管理 data_dir 下所有表的生命周期：
//   Open:  遍历 data_dir 子目录，加载每张表的 schema.json → 重建 TableSchema。
//   CreateTable: 创建子目录 + 写入 schema.json + 加入内存列表。
//
// 设计约束：
//   - 扫描时跳过隐藏目录（.开头）和非目录文件。
//   - 损坏的 schema.json（解析失败）静默跳过，不阻止数据库打开。
//     这是有意设计：运维时可以手动删除损坏表目录而不影响其他表。
//   - 表名即子目录名，不能重名（文件系统保证）。
//   - 当前不支持 DROP TABLE（需删除目录 + 同步删除，可能破坏其他 Connection
//     的 PartManager 缓存，尚未设计安全删除语义）。
//
// 线程安全：当前未加锁。调用者（Connection）应确保单线程访问。
// 未来多线程支持将在 Catalog 方法内部加锁。

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "wavedb/schema.h"
#include "wavedb/status.h"

namespace wavedb {

class Catalog {
  public:
    Catalog() = default;

    // 打开 data_dir，扫描并加载所有已存在的表。
    // data_dir 不存在时自动创建，返回空的 Catalog。
    static Result<Catalog> Open(std::string data_dir);

    // 创建新表：在 data_dir 下建子目录并写入 schema.json。
    // 同名表已存在返回 ALREADY_EXISTS。
    Status CreateTable(const TableSchema& schema);

    // 添加列：更新内存 schema + 重写 schema.json。
    // 表不存在返回 NOT_FOUND。
    Status AddColumn(std::string_view table_name, std::string field_name, ColumnType type, TimePrecision prec);

    // 删除列：从 schema 移除 + 重写 schema.json。
    // 列不存在返回 NOT_FOUND。旧 Part 的 .col 文件保留不删除。
    Status DropColumn(std::string_view table_name, std::string_view field_name);

    // 按名称查表，O(n) 线性扫描（n = 表数，通常 < 100）。
    // 返回 nullptr 表示未找到。
    const TableSchema* GetTable(std::string_view name) const;
    TableSchema* GetTable(std::string_view name);

    // 按索引访问表（用于遍历）。
    const TableSchema* GetTableByIndex(size_t i) const { return i < tables_.size() ? &tables_[i] : nullptr; }

    size_t table_count() const { return tables_.size(); }
    const std::string& data_dir() const { return data_dir_; }

  private:
    Status CreateTableDir(std::string_view table_name);
    Status WriteSchemaFile(const TableSchema& schema);

    std::string data_dir_;
    std::vector<TableSchema> tables_;  // 所有已加载的表 schema
};

}  // namespace wavedb
