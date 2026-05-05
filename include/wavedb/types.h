// WaveDB 基础类型定义。
//
// 设计决策：
//   - Timestamp 使用 int64_t 微秒纪元（Unix epoch µs），而非 time_t 或 chrono。
//     原因：(1) 整数比较/排序零开销 (2) 文件格式固定 8 字节 (3) 精度统一为微秒。
//   - Value 使用 std::variant<int64_t, double> 而非 class hierarchy。
//     原因：variant 是栈对象，无虚函数调用，无堆分配，适合热路径。
//     int64_t 同时承载 INT 和 TIMESTAMP，由 ColumnType 在解释时区分。
//   - ColumnType 仅三种（TIMESTAMP/FLOAT/INT），后续可扩展但保持枚举紧凑。
//     当前不支持 STRING/BLOB，以避免列文件变长、排序语义复杂化，
//     且时序场景几乎不需要字符串列。

#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "wavedb/status.h"

namespace wavedb {

// 列数据类型。存储格式为定长：TIMESTAMP→8B, FLOAT→8B, INT→8B。
// 定长存储是列式扫描的基础——列文件可按 sizeof * row_count 直接寻址。
enum class ColumnType : uint8_t {
    TIMESTAMP = 0,
    FLOAT,
    INT,
};

// 时间戳显示精度。
// 仅影响输出格式和部分解析行为，不影响存储（存储始终是微秒 int64_t）。
// 同一数据库的不同列可以有不同的显示精度。
enum class TimePrecision : uint8_t {
    DAY,     // 20230306
    HOUR,    // 20230306-12
    MINUTE,  // 20230306-12:25
    SECOND,  // 20230306-12:25:36
    MILLI,   // 20230306-12:25:36-534
    MICRO,   // 20230306-12:25:36-534008
};

// Part 合并策略。CREATE TABLE 时指定，后台 MergeScheduler 按此策略自动合并 Part。
enum class MergePolicy : uint8_t {
    NONE = 0,    // 不合并（默认）
    BY_HOUR,     // 按小时合并（同小时内的 Part 合并为一个）
    BY_DAY,      // 按天合并
    BY_MONTH,    // 按月合并
};

// 合并配置：策略 + 行数限制。
struct MergeConfig {
    MergePolicy policy = MergePolicy::NONE;
    int64_t max_rows_per_part = 0;  // 合并后单 Part 最大行数，0 = 不限制
};

// 微秒纪元时间戳。范围覆盖 +-292 年，满足所有时序场景。
// 选择微秒而非纳秒：8 字节足够表示 ±292 年微秒精度，
// 且金融/工业 Tick 数据通常不需要纳秒。
using Timestamp = int64_t;

// 通用数据值。用于字面量传递、扫描结果、Appender 缓冲。
//
// 设计选择：std::variant 而非继承体系。
//   - variant 是栈对象，遍历 Value 向量时无指针跳转（cache-friendly）。
//   - int64_t 同时承载 INT 和 TIMESTAMP（存储格式相同），
//     由 ColumnType 在读写时决定如何解释。
//   - 热路径允许的类型检查用 std::holds_alternative（编译期确定）。
using Value = std::variant<int64_t, double>;

// 返回 ColumnType 对应列值的定长存储字节数。
// 所有列类型当前均为 8 字节——简化列文件按恒定 stride 寻址。
inline constexpr size_t ColumnTypeSize(ColumnType t) {
    switch (t) {
        case ColumnType::TIMESTAMP:
            return sizeof(int64_t);
        case ColumnType::FLOAT:
            return sizeof(double);
        case ColumnType::INT:
            return sizeof(int64_t);
    }
    return 0;
}

// 将微秒时间戳格式化为人类可读字符串。
// 输出格式因精度而异，见 TimePrecision 枚举注释。
std::string FormatTimestamp(Timestamp ts, TimePrecision prec);

// 精度名与枚举互相转换（JSON schema 序列化/反序列化用）。
std::string_view TimePrecisionName(TimePrecision prec);
TimePrecision TimePrecisionFromName(std::string_view name);

// 合并策略名与枚举互相转换（JSON schema 序列化/反序列化用）。
std::string_view MergePolicyName(MergePolicy policy);
MergePolicy MergePolicyFromName(std::string_view name);

// 精度自适应：截断到指定精度 / 扩展到周期末尾。
Timestamp TruncateToPrecision(Timestamp ts, TimePrecision prec);
Timestamp ExpandToPeriodEnd(Timestamp ts, TimePrecision prec);

// 推断时间戳字面量的精度（用于 WHERE 精度自适应）。
TimePrecision TimestampLiteralPrecision(std::string_view s);

// 将时间字符串解析为微秒时间戳。
// 输入格式与 FormatTimestamp 一致。
//
// 若输入精度低于列精度，缺失部分自动补零：
//   "20230306"           → 20230306-00:00:00-000000
//   "20230306-12:25:36"  → 20230306-12:25:36-000000
//
// col_prec 参数预留用于未来校验（v0.2 尚未强制输入精度 ≤ 列精度）。
Result<Timestamp> ParseTimestamp(std::string_view str, TimePrecision prec);

}  // namespace wavedb
