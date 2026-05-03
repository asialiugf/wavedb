#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "wavedb/status.h"

namespace wavedb {

enum class ColumnType : uint8_t {
    kTimestamp = 0,
    kFloat,
    kInt,
};

// 时间戳显示精度，CREATE TABLE 时指定。
enum class TimePrecision : uint8_t {
    DAY,     // 20230306
    HOUR,    // 20230306-12
    MINUTE,  // 20230306-12:25
    SECOND,  // 20230306-12:25:36
    MILLI,   // 20230306-12:25:36-534
    MICRO,   // 20230306-12:25:36-534008
};

// 微秒纪元时间戳。范围覆盖 +-292 年，满足所有时序场景。
using Timestamp = int64_t;

// 用于传递单个数据值（字面量、扫描结果等）。
// int64_t 同时承载 INT 和 TIMESTAMP——ColumnType 决定如何解释。
using Value = std::variant<int64_t, double>;

// 返回 ColumnType 对应的定长存储字节数。
inline constexpr size_t ColumnTypeSize(ColumnType t) {
    switch (t) {
        case ColumnType::kTimestamp:
            return sizeof(int64_t);
        case ColumnType::kFloat:
            return sizeof(double);
        case ColumnType::kInt:
            return sizeof(int64_t);
    }
    return 0;
}

// 将微秒时间戳格式化为人类可读字符串。
std::string FormatTimestamp(Timestamp ts, TimePrecision prec);

// 精度名映射（JSON 反序列化用）。
std::string_view TimePrecisionName(TimePrecision prec);
TimePrecision TimePrecisionFromName(std::string_view name);

// 将时间字符串解析为微秒时间戳。输入格式与 FormatTimestamp 一致。
// 若输入精度低于列精度，缺失部分自动补零。
// 示例（列精度 MICRO）：
//   "20230306"           → 20230306-00:00:00-000000
//   "20230306-12:25:36"  → 20230306-12:25:36-000000
Result<Timestamp> ParseTimestamp(std::string_view str, TimePrecision prec);

}  // namespace wavedb
