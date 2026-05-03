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

    // 打开 data_dir 下的所有表，加载 schema.json。
    // 若 data_dir 不存在则自动创建。
    static Result<Catalog> Open(std::string data_dir);

    // 创建新表：建目录 + 写 schema.json。表已存在则返回 kAlreadyExists。
    Status CreateTable(const TableSchema& schema);

    // 按名称查表。未找到返回 nullptr。
    const TableSchema* GetTable(std::string_view name) const;
    TableSchema* GetTable(std::string_view name);

    size_t table_count() const { return tables_.size(); }
    const std::string& data_dir() const { return data_dir_; }

  private:
    Status CreateTableDir(std::string_view table_name);
    Status WriteSchemaFile(const TableSchema& schema);

    std::string data_dir_;
    std::vector<TableSchema> tables_;
};

}  // namespace wavedb
