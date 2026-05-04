// Schema JSON 序列化/反序列化。
//
// 设计决策：手写解析器而非依赖 nlohmann/json 或 RapidJSON。
//   原因：
//     (1) schema.json 是简单、格式固定的文件，不需要通用 JSON 库。
//     (2) 零外部依赖——减少链接复杂性和二进制大小。
//     (3) 解析器仅 ~100 行，维护成本远低于引入第三方库。
//     (4) 未知字段自动跳过——保证 schema.json 向前兼容。
//
// FromJson 的容错策略：
//   未知字段 → 跳过（字符串/对象/数组/基本类型均能处理）。
//   丢失 "name" 字段 → PARSE_ERROR。
//   丢失 "columns" 字段 → 允许（空表 schema，0 列）。
//   列定义缺少 name 或 type → PARSE_ERROR。
//   列精度缺失 → 默认 MICRO。

#include "wavedb/schema.h"

#include <charconv>
#include <cstring>

namespace wavedb {

// ---- 类型名映射 ----

static std::string_view ColumnTypeName(ColumnType t) {
    switch (t) {
        case ColumnType::TIMESTAMP:
            return "TIMESTAMP";
        case ColumnType::FLOAT:
            return "FLOAT";
        case ColumnType::INT:
            return "INT";
    }
    return "UNKNOWN";
}

static ColumnType ColumnTypeFromName(std::string_view name) {
    if (name == "TIMESTAMP") return ColumnType::TIMESTAMP;
    if (name == "FLOAT") return ColumnType::FLOAT;
    if (name == "INT") return ColumnType::INT;
    return ColumnType::INT;  // 未知类型默认 INT（防御性）
}

// ---- ToJson ----

std::string TableSchema::ToJson() const {
    std::string json;
    json.reserve(256);
    json += "{\n";
    json += "  \"name\": \"" + name_ + "\",\n";
    json += "  \"columns\": [\n";
    for (size_t i = 0; i < columns_.size(); ++i) {
        json += "    {\"name\": \"";
        json += columns_[i].name;
        json += "\", \"type\": \"";
        json += ColumnTypeName(columns_[i].type);
        // precision 仅在 TIMESTAMP 列写入，减少非 TIMESTAMP 列的冗余字段。
        if (columns_[i].type == ColumnType::TIMESTAMP) {
            json += "\", \"precision\": \"";
            json += TimePrecisionName(columns_[i].precision);
        }
        json += "\"}";
        if (i + 1 < columns_.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    // 合并配置（仅在非 NONE 时写入）
    if (merge_config_.policy != MergePolicy::NONE) {
        json += ",\n";
        json += "  \"merge\": {\n";
        json += "    \"policy\": \"";
        json += MergePolicyName(merge_config_.policy);
        json += "\"";
        if (merge_config_.max_rows_per_part > 0) {
            json += ",\n    \"max_rows_per_part\": ";
            json += std::to_string(merge_config_.max_rows_per_part);
        }
        json += "\n  }";
    }
    json += "\n}";
    return json;
}

// ---- FromJson（极简手写解析器） ----
//
// 支持：字符串、对象、数组、基本类型值。
// 不支持：转义字符（schema.json 不含控制字符）、Unicode 转义。

static const char* SkipWS(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

static const char* ReadString(const char* p, const char* end, std::string& out) {
    p = SkipWS(p, end);
    if (p >= end || *p != '"') return nullptr;
    ++p;
    out.clear();
    while (p < end && *p != '"') {
        out += *p;
        ++p;
    }
    if (p >= end) return nullptr;  // 未闭合的字符串
    return p + 1;  // 跳过结束的 "
}

Result<TableSchema> TableSchema::FromJson(std::string_view json) {
    const char* p = json.data();
    const char* end = p + json.size();

    p = SkipWS(p, end);
    if (p >= end || *p != '{') return Status(StatusCode::PARSE_ERROR, "expected '{'");
    ++p;

    TableSchema schema;

    while (p < end) {
        p = SkipWS(p, end);
        if (p >= end) break;
        if (*p == '}') break;  // 顶层对象结束

        std::string key;
        p = ReadString(p, end, key);
        if (!p) return Status(StatusCode::PARSE_ERROR, "expected key string");

        p = SkipWS(p, end);
        if (p >= end || *p != ':') return Status(StatusCode::PARSE_ERROR, "expected ':'");
        ++p;

        p = SkipWS(p, end);

        if (key == "name") {
            std::string val;
            p = ReadString(p, end, val);
            if (!p) return Status(StatusCode::PARSE_ERROR, "expected table name");
            schema.set_name(std::move(val));
        } else if (key == "columns") {
            if (p >= end || *p != '[') return Status(StatusCode::PARSE_ERROR, "expected '['");
            ++p;

            while (true) {
                p = SkipWS(p, end);
                if (p >= end) return Status(StatusCode::PARSE_ERROR, "unexpected end in columns");
                if (*p == ']') {
                    ++p;
                    break;
                }

                if (*p != '{') return Status(StatusCode::PARSE_ERROR, "expected '{' in columns");
                ++p;

                std::string col_name;
                std::string col_type;
                TimePrecision col_prec = TimePrecision::MICRO;

                // 读取列对象的字段直到遇到 '}'
                while (true) {
                    p = SkipWS(p, end);
                    if (p >= end) return Status(StatusCode::PARSE_ERROR, "unexpected end in column");
                    if (*p == '}') break;

                    std::string fkey;
                    p = ReadString(p, end, fkey);
                    if (!p) return Status(StatusCode::PARSE_ERROR, "expected column key");

                    p = SkipWS(p, end);
                    if (p >= end || *p != ':') return Status(StatusCode::PARSE_ERROR, "expected ':'");
                    ++p;

                    std::string fval;
                    p = ReadString(p, end, fval);
                    if (!p) return Status(StatusCode::PARSE_ERROR, "expected column value");

                    if (fkey == "name")
                        col_name = std::move(fval);
                    else if (fkey == "type")
                        col_type = std::move(fval);
                    else if (fkey == "precision")
                        col_prec = TimePrecisionFromName(fval);

                    p = SkipWS(p, end);
                    if (p < end && *p == ',') ++p;  // 跳过字段间逗号
                }

                if (col_name.empty() || col_type.empty())
                    return Status(StatusCode::PARSE_ERROR, "column missing name or type");

                schema.AddColumn(std::move(col_name), ColumnTypeFromName(col_type), col_prec);

                p = SkipWS(p, end);
                if (p >= end) return Status(StatusCode::PARSE_ERROR, "unexpected end after column");
                ++p;  // 跳过 '}'（while 循环在 '}' 处 break 时未消费）

                p = SkipWS(p, end);
                if (p < end && *p == ',') ++p;  // 跳过列间逗号
            }
        } else if (key == "merge") {
            // 解析 "merge": { "policy": "by_day", "max_rows_per_part": 1000000 }
            if (p >= end || *p != '{') return Status(StatusCode::PARSE_ERROR, "expected '{' for merge config");
            ++p;
            while (true) {
                p = SkipWS(p, end);
                if (p >= end) return Status(StatusCode::PARSE_ERROR, "unexpected end in merge config");
                if (*p == '}') {
                    ++p;
                    break;
                }
                std::string mkey;
                p = ReadString(p, end, mkey);
                if (!p) return Status(StatusCode::PARSE_ERROR, "expected merge config key");
                p = SkipWS(p, end);
                if (p >= end || *p != ':') return Status(StatusCode::PARSE_ERROR, "expected ':'");
                ++p;
                p = SkipWS(p, end);
                if (mkey == "policy") {
                    std::string pval;
                    p = ReadString(p, end, pval);
                    if (!p) return Status(StatusCode::PARSE_ERROR, "expected merge policy value");
                    schema.merge_config_.policy = MergePolicyFromName(pval);
                } else if (mkey == "max_rows_per_part") {
                    // 直接解析整数字面量
                    int64_t val = 0;
                    bool neg = false;
                    if (p < end && *p == '-') { neg = true; ++p; }
                    while (p < end && *p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; }
                    schema.merge_config_.max_rows_per_part = neg ? -val : val;
                } else {
                    // 未知 merge 字段 → 跳过
                    if (*p == '"') { std::string ignored; p = ReadString(p, end, ignored); }
                    else if (*p == '{' || *p == '[') { int d = 1; ++p; while (p < end && d > 0) { if (*p == '{' || *p == '[') ++d; else if (*p == '}' || *p == ']') --d; ++p; } }
                    else { while (p < end && *p != ',' && *p != '}' && *p != ']') ++p; }
                }
                p = SkipWS(p, end);
                if (p < end && *p == ',') ++p;
            }
        } else {
            // 未知字段 → 跳过其值，保证向前兼容
            if (*p == '"') {
                std::string ignored;
                p = ReadString(p, end, ignored);
                if (!p) return Status(StatusCode::PARSE_ERROR, "expected string value");
            } else if (*p == '{' || *p == '[') {
                // 跳过嵌套对象/数组——统计括号深度
                int depth = 1;
                ++p;
                while (p < end && depth > 0) {
                    if (*p == '{' || *p == '[')
                        ++depth;
                    else if (*p == '}' || *p == ']')
                        --depth;
                    ++p;
                }
            } else {
                // 跳过基本类型值（数字、布尔、null）
                while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
            }
        }

        p = SkipWS(p, end);
        if (p < end && *p == ',') ++p;
    }

    if (schema.name().empty()) return Status(StatusCode::PARSE_ERROR, "table schema missing name");

    return schema;
}

}  // namespace wavedb
