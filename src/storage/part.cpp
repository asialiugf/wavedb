// Part 实现：不可变数据分区的创建、打开与读取。
//
// Part 目录结构：
//   parts/001/
//     meta.json  → 时间范围 + 行数
//     ts.col     → TIMESTAMP 列数据（裸 int64_t 数组）
//     price.col  → FLOAT 列数据（裸 double 数组）
//     volume.col → INT 列数据（裸 int64_t 数组）
//
// meta.json 包含 min_ts / max_ts 供 PartManager 时间裁剪使用。
// 但行级的时间戳过滤仍需逐行检查（Part 内部数据不排序）。

#include "src/storage/part.h"

#include <sys/stat.h>

#include <cstdio>
#include <span>

#include "src/storage/column_file.h"
#include "wavedb/schema.h"

namespace wavedb {

// ---- meta.json 写入 ----

static std::string MetaJson(
    int64_t min_ts, int64_t max_ts, size_t row_count, TimePrecision prec) {
    auto min_str = FormatTimestamp(min_ts, prec);
    auto max_str = FormatTimestamp(max_ts, prec);
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "{\n"
        "  \"min_ts\": %lld,\n"
        "  \"min_ts_str\": \"%s\",\n"
        "  \"max_ts\": %lld,\n"
        "  \"max_ts_str\": \"%s\",\n"
        "  \"row_count\": %zu\n"
        "}\n",
        (long long)min_ts, min_str.c_str(), (long long)max_ts, max_str.c_str(), row_count);
    return buf;
}

static std::string MetaJsonWithBoundary(
    int64_t min_ts, int64_t max_ts, size_t row_count, int64_t merge_boundary, TimePrecision prec) {
    auto min_str = FormatTimestamp(min_ts, prec);
    auto max_str = FormatTimestamp(max_ts, prec);
    char buf[640];
    std::snprintf(
        buf, sizeof(buf),
        "{\n"
        "  \"min_ts\": %lld,\n"
        "  \"min_ts_str\": \"%s\",\n"
        "  \"max_ts\": %lld,\n"
        "  \"max_ts_str\": \"%s\",\n"
        "  \"row_count\": %zu,\n"
        "  \"merge_boundary\": %lld\n"
        "}\n",
        (long long)min_ts, min_str.c_str(), (long long)max_ts, max_str.c_str(), row_count,
        (long long)merge_boundary);
    return buf;
}

static Status WriteMetaJson(
    const std::string& path, int64_t min_ts, int64_t max_ts, size_t row_count, TimePrecision prec) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return Status(StatusCode::IO_ERROR, "cannot write meta.json: " + path);
    auto content = MetaJson(min_ts, max_ts, row_count, prec);
    size_t n = std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    if (n != content.size()) return Status(StatusCode::IO_ERROR, "write meta.json truncated: " + path);
    return Status::OK();
}

// ---- Part::Create ----

Result<Part> Part::Create(
    std::string part_dir,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns,
    int64_t min_ts,
    int64_t max_ts) {
    // 创建 Part 目录
    if (::mkdir(part_dir.c_str(), 0755) != 0) return Status(StatusCode::IO_ERROR, "mkdir failed: " + part_dir);

    size_t ncols = schema.column_count();
    if (columns.size() != ncols) return Status(StatusCode::INVALID_ARGUMENT, "column count mismatch");

    size_t nrows = columns.empty() ? 0 : columns[0].size();

    // 逐列写入 .col 文件
    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = schema.column_at(ci);
        std::string col_path = part_dir + "/" + col_def.name + ".col";

        auto cf = ColumnFile::Open(col_path, col_def.type);
        if (!cf.ok()) return cf.status;

        // Value→ 具体类型转换后批量写入
        if (col_def.type == ColumnType::FLOAT) {
            std::vector<double> buf;
            buf.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) buf.push_back(std::get<double>(columns[ci][r]));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        } else {
            std::vector<int64_t> buf;
            buf.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) buf.push_back(std::get<int64_t>(columns[ci][r]));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        }
        cf->Close();
    }

    // 获取时间戳列的精度（用于 meta.json 人类可读时间）
    TimePrecision ts_prec = TimePrecision::MICRO;
    for (size_t ci = 0; ci < ncols; ++ci)
        if (schema.column_at(ci).type == ColumnType::TIMESTAMP) { ts_prec = schema.column_at(ci).precision; break; }

    // 最后写入 meta.json（原子性标记 Part 完成）
    std::string meta_path = part_dir + "/meta.json";
    Status s = WriteMetaJson(meta_path, min_ts, max_ts, nrows, ts_prec);
    if (!s.ok()) return s;

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
    part.row_count_ = nrows;
    part.schema_ = schema;
    return part;
}

// ---- Part::Open ----
//
// meta.json 的极简 JSON 解析器——与 schema.cpp 中的解析器功能类似，
// 但独立实现以避免跨模块依赖（Part::Open 只需要解析 3 个整数字段）。

namespace {

const char* SkipWS(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

const char* ReadString(const char* p, const char* end, std::string& out) {
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

// 解析有符号 int64。消费指针 p 到下一个非数字字符。
int64_t ParseInt(const char*& p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        ++p;
    }
    int64_t val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        ++p;
    }
    return neg ? -val : val;
}

}  // namespace

Result<Part> Part::Open(std::string part_dir, const TableSchema& schema) {
    std::string meta_path = part_dir + "/meta.json";
    FILE* f = std::fopen(meta_path.c_str(), "rb");
    if (!f) return Status(StatusCode::IO_ERROR, "cannot open meta.json: " + meta_path);

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string json(sz, '\0');
    if (sz > 0) {
        size_t n = std::fread(json.data(), 1, sz, f);
        (void)n;
    }
    std::fclose(f);

    // 解析 meta.json
    const char* p = json.data();
    const char* end = p + json.size();

    p = SkipWS(p, end);
    if (p >= end || *p != '{') return Status(StatusCode::PARSE_ERROR, "expected '{' in meta.json");
    ++p;

    int64_t min_ts = 0, max_ts = 0, merge_boundary = 0;
    size_t row_count = 0;
    bool has_min = false, has_max = false, has_rows = false;

    while (p < end) {
        p = SkipWS(p, end);
        if (p >= end || *p == '}') break;

        std::string key;
        p = ReadString(p, end, key);
        if (!p) return Status(StatusCode::PARSE_ERROR, "expected key in meta.json");

        p = SkipWS(p, end);
        if (p >= end || *p != ':') return Status(StatusCode::PARSE_ERROR, "expected ':' in meta.json");
        ++p;
        p = SkipWS(p, end);

        if (key == "min_ts") {
            min_ts = ParseInt(p, end);
            has_min = true;
        } else if (key == "max_ts") {
            max_ts = ParseInt(p, end);
            has_max = true;
        } else if (key == "row_count") {
            row_count = static_cast<size_t>(ParseInt(p, end));
            has_rows = true;
        } else if (key == "merge_boundary") {
            merge_boundary = ParseInt(p, end);
        } else {
            // 未知字段 → 跳过
            if (*p == '"') {
                std::string ignored;
                p = ReadString(p, end, ignored);
            } else {
                while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
            }
        }

        p = SkipWS(p, end);
        if (p < end && *p == ',') ++p;
    }

    // 三个字段缺一不可
    if (!has_min || !has_max || !has_rows) return Status(StatusCode::PARSE_ERROR, "meta.json missing fields");

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
    part.merge_boundary_ = merge_boundary;
    part.row_count_ = row_count;
    part.schema_ = schema;
    return part;
}

// ---- Part::ReadColumn ----

Result<std::vector<Value>> Part::ReadColumn(int col_idx, ColumnType type) const {
    const auto& col_def = schema_.column_at(col_idx);
    std::string col_path = dir_ + "/" + col_def.name + ".col";

    auto cf = ColumnFile::Open(col_path, type);
    if (!cf.ok()) return cf.status;

    // 若列文件为空（0 行）但 Part 有数据，说明该列是 ALTER TABLE ADD FIELD
    // 之后新增的，旧 Part 中不存在该列的 .col 文件。
    // "a+b" 模式会创建空文件，返回 row_count 个默认值（0 或 0.0）。
    if (cf->row_count() == 0 && row_count_ > 0) {
        std::vector<Value> out;
        out.reserve(row_count_);
        if (type == ColumnType::FLOAT) {
            for (size_t i = 0; i < row_count_; ++i) out.push_back(0.0);
        } else {
            for (size_t i = 0; i < row_count_; ++i) out.push_back(int64_t(0));
        }
        return out;
    }

    // 根据类型读取并包装为 Value 向量
    if (type == ColumnType::FLOAT) {
        auto data = cf->ReadAllFloat64();
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(data->size());
        for (double d : *data) out.push_back(d);
        return out;
    } else {
        auto data = cf->ReadAllInt64();
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(data->size());
        for (int64_t v : *data) out.push_back(v);
        return out;
    }
}

// ---- Part::ReadColumnRange ----

Result<std::vector<Value>> Part::ReadColumnRange(int col_idx, ColumnType type, size_t start, size_t count) const {
    if (start + count > row_count_)
        return Status(
            StatusCode::INVALID_ARGUMENT,
            "range [" + std::to_string(start) + ", " + std::to_string(start + count) + ") exceeds row_count " +
                std::to_string(row_count_));

    const auto& col_def = schema_.column_at(col_idx);
    std::string col_path = dir_ + "/" + col_def.name + ".col";

    auto cf = ColumnFile::Open(col_path, type);
    if (!cf.ok()) return cf.status;

    // 缺失列（ALTER TABLE ADD 后旧 Part）：返回 count 个默认值
    if (cf->row_count() == 0 && row_count_ > 0) {
        std::vector<Value> out;
        out.reserve(count);
        if (type == ColumnType::FLOAT) {
            for (size_t i = 0; i < count; ++i) out.push_back(0.0);
        } else {
            for (size_t i = 0; i < count; ++i) out.push_back(int64_t(0));
        }
        return out;
    }

    if (type == ColumnType::FLOAT) {
        auto data = cf->ReadRangeFloat64(start, count);
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(data->size());
        for (double d : *data) out.push_back(d);
        return out;
    } else {
        auto data = cf->ReadRangeInt64(start, count);
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(data->size());
        for (int64_t v : *data) out.push_back(v);
        return out;
    }
}

// ---- Part::WriteColumn ----
//
// 写单个 .col 文件：先写 .col.tmp，再 rename 为 .col。
// 原子性：rename 是 POSIX 原子操作，Reader 要么看到旧文件（或无文件→默认值），
// 要么看到完整的新文件，不会看到半写状态。
Status Part::WriteColumn(std::string_view col_name, ColumnType type, const std::vector<Value>& values) const {
    if (values.size() != row_count_)
        return Status(
            StatusCode::INVALID_ARGUMENT,
            "values count mismatch: " + std::to_string(values.size()) + " vs " + std::to_string(row_count_));

    std::string col_path = dir_ + "/" + std::string(col_name) + ".col";
    std::string tmp_path = col_path + ".tmp";

    // 写入临时文件
    {
        auto cf = ColumnFile::Open(tmp_path, type);
        if (!cf.ok()) return cf.status;

        if (type == ColumnType::FLOAT) {
            std::vector<double> buf;
            buf.reserve(values.size());
            for (auto& v : values) buf.push_back(std::get<double>(v));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        } else {
            std::vector<int64_t> buf;
            buf.reserve(values.size());
            for (auto& v : values) buf.push_back(std::get<int64_t>(v));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        }
        cf->Close();
    }

    // 原子 rename
    if (std::rename(tmp_path.c_str(), col_path.c_str()) != 0)
        return Status(StatusCode::IO_ERROR, "rename failed: " + tmp_path);

    return Status::OK();
}

}  // namespace wavedb
