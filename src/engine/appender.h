#pragma once

#include <vector>

#include "src/catalog/schema.h"
#include "src/common/status.h"
#include "src/common/types.h"
#include "src/storage/column_file.h"

namespace wavedb {

class Appender {
  public:
    Appender() = default;
    Appender(const TableSchema* schema, std::vector<ColumnFile> files);

    Appender(Appender&&) = default;
    Appender& operator=(Appender&&) = default;

    ~Appender();

    // 追加一行。values 数量必须等于列数，类型必须匹配。
    Status AppendRow(const std::vector<Value>& row);

    // variadic 便捷重载：AppendRow(ts, price, volume);
    template <typename... Args>
    Status AppendRow(Args... args) {
        return AppendRow(std::vector<Value>{Value(args)...});
    }

    // 强制刷盘。
    Status Flush();

    // 关闭 appender，刷盘并释放文件。
    Status Close();

    size_t row_count() const { return row_count_; }

  private:
    const TableSchema* schema_ = nullptr;
    std::vector<ColumnFile> files_;
    size_t row_count_ = 0;
};

}  // namespace wavedb
