#include "wavedb/schema.h"

#include <charconv>
#include <cstring>

namespace wavedb {

// ---- 类型名映射 ----

static std::string_view ColumnTypeName(ColumnType t) {
    switch (t) {
        case ColumnType::kTimestamp:
            return "TIMESTAMP";
        case ColumnType::kFloat:
            return "FLOAT";
        case ColumnType::kInt:
            return "INT";
    }
    return "UNKNOWN";
}

static ColumnType ColumnTypeFromName(std::string_view name) {
    if (name == "TIMESTAMP") return ColumnType::kTimestamp;
    if (name == "FLOAT") return ColumnType::kFloat;
    if (name == "INT") return ColumnType::kInt;
    return ColumnType::kInt;
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
        if (columns_[i].type == ColumnType::kTimestamp) {
            json += "\", \"precision\": \"";
            json += TimePrecisionName(columns_[i].precision);
        }
        json += "\"}";
        if (i + 1 < columns_.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    json += "}";
    return json;
}

// ---- FromJson（极简手写解析器） ----

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
    if (p >= end) return nullptr;
    return p + 1;
}

Result<TableSchema> TableSchema::FromJson(std::string_view json) {
    const char* p = json.data();
    const char* end = p + json.size();

    p = SkipWS(p, end);
    if (p >= end || *p != '{') return Status(StatusCode::kParseError, "expected '{'");
    ++p;

    TableSchema schema;

    while (p < end) {
        p = SkipWS(p, end);
        if (p >= end) break;
        if (*p == '}') break;

        std::string key;
        p = ReadString(p, end, key);
        if (!p) return Status(StatusCode::kParseError, "expected key string");

        p = SkipWS(p, end);
        if (p >= end || *p != ':') return Status(StatusCode::kParseError, "expected ':'");
        ++p;

        p = SkipWS(p, end);

        if (key == "name") {
            std::string val;
            p = ReadString(p, end, val);
            if (!p) return Status(StatusCode::kParseError, "expected table name");
            schema.set_name(std::move(val));
        } else if (key == "columns") {
            if (p >= end || *p != '[') return Status(StatusCode::kParseError, "expected '['");
            ++p;

            while (true) {
                p = SkipWS(p, end);
                if (p >= end) return Status(StatusCode::kParseError, "unexpected end in columns");
                if (*p == ']') {
                    ++p;
                    break;
                }

                if (*p != '{') return Status(StatusCode::kParseError, "expected '{' in columns");
                ++p;

                std::string col_name;
                std::string col_type;
                TimePrecision col_prec = TimePrecision::MICRO;

                // 读取字段直到遇到 '}'
                while (true) {
                    p = SkipWS(p, end);
                    if (p >= end) return Status(StatusCode::kParseError, "unexpected end in column");
                    if (*p == '}') break;

                    std::string fkey;
                    p = ReadString(p, end, fkey);
                    if (!p) return Status(StatusCode::kParseError, "expected column key");

                    p = SkipWS(p, end);
                    if (p >= end || *p != ':') return Status(StatusCode::kParseError, "expected ':'");
                    ++p;

                    std::string fval;
                    p = ReadString(p, end, fval);
                    if (!p) return Status(StatusCode::kParseError, "expected column value");

                    if (fkey == "name")
                        col_name = std::move(fval);
                    else if (fkey == "type")
                        col_type = std::move(fval);
                    else if (fkey == "precision")
                        col_prec = TimePrecisionFromName(fval);

                    p = SkipWS(p, end);
                    if (p < end && *p == ',') ++p;
                }

                if (col_name.empty() || col_type.empty())
                    return Status(StatusCode::kParseError, "column missing name or type");

                schema.AddColumn(std::move(col_name), ColumnTypeFromName(col_type), col_prec);

                p = SkipWS(p, end);
                if (p >= end) return Status(StatusCode::kParseError, "unexpected end after column");
                ++p;  // 跳过 '}'（while 循环在 '}' 处 break，未消费）

                p = SkipWS(p, end);
                if (p < end && *p == ',') ++p;
            }
        } else {
            if (*p == '"') {
                std::string ignored;
                p = ReadString(p, end, ignored);
                if (!p) return Status(StatusCode::kParseError, "expected string value");
            } else if (*p == '{' || *p == '[') {
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
                while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
            }
        }

        p = SkipWS(p, end);
        if (p < end && *p == ',') ++p;
    }

    if (schema.name().empty()) return Status(StatusCode::kParseError, "table schema missing name");

    return schema;
}

}  // namespace wavedb
