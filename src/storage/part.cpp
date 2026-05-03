#include "src/storage/part.h"

#include <sys/stat.h>

#include <cstdio>
#include <span>

#include "src/storage/column_file.h"
#include "wavedb/schema.h"

namespace wavedb {

// ---- meta.json 写 ----

static std::string MetaJson(int64_t min_ts, int64_t max_ts, size_t row_count) {
    char buf[256];
    std::snprintf(
        buf, sizeof(buf),
        "{\n"
        "  \"min_ts\": %lld,\n"
        "  \"max_ts\": %lld,\n"
        "  \"row_count\": %zu\n"
        "}\n",
        (long long)min_ts, (long long)max_ts, row_count);
    return buf;
}

static Status WriteMetaJson(const std::string& path, int64_t min_ts, int64_t max_ts, size_t row_count) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return Status(StatusCode::kIOError, "cannot write meta.json: " + path);
    auto content = MetaJson(min_ts, max_ts, row_count);
    size_t n = std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    if (n != content.size()) return Status(StatusCode::kIOError, "write meta.json truncated: " + path);
    return Status::OK();
}

// ---- Part::Create ----

Result<Part> Part::Create(
    std::string part_dir,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns,
    int64_t min_ts,
    int64_t max_ts) {
    if (::mkdir(part_dir.c_str(), 0755) != 0) return Status(StatusCode::kIOError, "mkdir failed: " + part_dir);

    size_t ncols = schema.column_count();
    if (columns.size() != ncols) return Status(StatusCode::kInvalidArgument, "column count mismatch");

    size_t nrows = columns.empty() ? 0 : columns[0].size();

    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = schema.column_at(ci);
        std::string col_path = part_dir + "/" + col_def.name + ".col";

        auto cf = ColumnFile::Open(col_path, col_def.type);
        if (!cf.ok()) return cf.status;

        if (col_def.type == ColumnType::kFloat) {
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

    std::string meta_path = part_dir + "/meta.json";
    Status s = WriteMetaJson(meta_path, min_ts, max_ts, nrows);
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

// 极简 JSON 解析器（SkipWS / ReadString / ParseNumber）
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
    if (!f) return Status(StatusCode::kIOError, "cannot open meta.json: " + meta_path);

    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    std::string json(sz, '\0');
    if (sz > 0) {
        size_t n = std::fread(json.data(), 1, sz, f);
        (void)n;
    }
    std::fclose(f);

    // 解析 JSON
    const char* p = json.data();
    const char* end = p + json.size();

    p = SkipWS(p, end);
    if (p >= end || *p != '{') return Status(StatusCode::kParseError, "expected '{' in meta.json");
    ++p;

    int64_t min_ts = 0, max_ts = 0;
    size_t row_count = 0;
    bool has_min = false, has_max = false, has_rows = false;

    while (p < end) {
        p = SkipWS(p, end);
        if (p >= end || *p == '}') break;

        std::string key;
        p = ReadString(p, end, key);
        if (!p) return Status(StatusCode::kParseError, "expected key in meta.json");

        p = SkipWS(p, end);
        if (p >= end || *p != ':') return Status(StatusCode::kParseError, "expected ':' in meta.json");
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
        } else {
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

    if (!has_min || !has_max || !has_rows) return Status(StatusCode::kParseError, "meta.json missing fields");

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
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

    if (type == ColumnType::kFloat) {
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

}  // namespace wavedb
