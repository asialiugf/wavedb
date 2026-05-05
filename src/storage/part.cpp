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
#include "third_party/yyjson.h"
#include "wavedb/schema.h"

namespace wavedb {

// ---- meta.json 写入（yyjson）----

// 构建 meta.json 的 yyjson mutable doc，返回 JSON 字符串（需 free）。
// 调用者负责 free(json_str)。
static char* BuildMetaJson(
    int64_t min_ts, int64_t max_ts, size_t row_count, size_t merge_offset,
    int64_t merge_boundary, TimePrecision prec) {
    auto min_str = FormatTimestamp(min_ts, prec);
    auto max_str = FormatTimestamp(max_ts, prec);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "min_ts", min_ts);
    yyjson_mut_obj_add_str(doc, root, "min_ts_str", min_str.c_str());
    yyjson_mut_obj_add_int(doc, root, "max_ts", max_ts);
    yyjson_mut_obj_add_str(doc, root, "max_ts_str", max_str.c_str());
    yyjson_mut_obj_add_int(doc, root, "row_count", static_cast<int64_t>(row_count));
    yyjson_mut_obj_add_int(doc, root, "merge_offset", static_cast<int64_t>(merge_offset));
    if (merge_boundary != 0)
        yyjson_mut_obj_add_int(doc, root, "merge_boundary", merge_boundary);

    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    yyjson_mut_doc_free(doc);
    return json;
}

static Status WriteMetaJson(
    const std::string& path, int64_t min_ts, int64_t max_ts, size_t row_count, TimePrecision prec) {
    char* json = BuildMetaJson(min_ts, max_ts, row_count, 0, 0, prec);
    if (!json) return Status(StatusCode::INTERNAL, "yyjson build failed");

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { free(json); return Status(StatusCode::IO_ERROR, "cannot write: " + path); }
    size_t len = strlen(json);
    size_t n = std::fwrite(json, 1, len, f);
    std::fclose(f);
    free(json);
    if (n != len) return Status(StatusCode::IO_ERROR, "write truncated: " + path);
    return Status::OK();
}

static Status RewriteMetaJson(
    const std::string& path, int64_t min_ts, int64_t max_ts, size_t row_count,
    size_t merge_offset, int64_t merge_boundary, TimePrecision prec) {
    char* json = BuildMetaJson(min_ts, max_ts, row_count, merge_offset, merge_boundary, prec);
    if (!json) return Status(StatusCode::INTERNAL, "yyjson build failed");

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { free(json); return Status(StatusCode::IO_ERROR, "cannot write: " + path); }
    size_t len = strlen(json);
    size_t n = std::fwrite(json, 1, len, f);
    std::fclose(f);
    free(json);
    if (n != len) return Status(StatusCode::IO_ERROR, "write truncated: " + path);
    return Status::OK();
}

// ---- Part::Create ----
// 创建新 Part：写列数据 + meta.json。meta.json 的原子性标记 Part 是否完成。
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

        auto cf = ColumnFile::Open(col_path, col_def.type, /*exclusive=*/true);
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
    part.merge_offset_ = 0;
    part.schema_ = schema;
    return part;
}

// ---- Part::Open ----
Result<Part> Part::Open(std::string part_dir, const TableSchema& schema) {
    std::string meta_path = part_dir + "/meta.json";

    yyjson_doc* doc = yyjson_read_file(meta_path.c_str(), YYJSON_READ_ALLOW_COMMENTS, nullptr, nullptr);
    if (!doc) return Status(StatusCode::PARSE_ERROR, "cannot parse meta.json: " + meta_path);

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || yyjson_get_type(root) != YYJSON_TYPE_OBJ) {
        yyjson_doc_free(doc);
        return Status(StatusCode::PARSE_ERROR, "meta.json root is not object");
    }

    yyjson_val* v_min = yyjson_obj_get(root, "min_ts");
    yyjson_val* v_max = yyjson_obj_get(root, "max_ts");
    yyjson_val* v_rows = yyjson_obj_get(root, "row_count");
    if (!v_min || !v_max || !v_rows) {
        yyjson_doc_free(doc);
        return Status(StatusCode::PARSE_ERROR, "meta.json missing required fields");
    }

    int64_t min_ts = yyjson_get_sint(v_min);
    int64_t max_ts = yyjson_get_sint(v_max);
    size_t row_count = static_cast<size_t>(yyjson_get_sint(v_rows));

    size_t merge_offset = 0;
    yyjson_val* v_off = yyjson_obj_get(root, "merge_offset");
    if (v_off) merge_offset = static_cast<size_t>(yyjson_get_sint(v_off));

    int64_t merge_boundary = 0;
    yyjson_val* v_bnd = yyjson_obj_get(root, "merge_boundary");
    if (v_bnd) merge_boundary = yyjson_get_sint(v_bnd);

    yyjson_doc_free(doc);

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
    part.merge_boundary_ = merge_boundary;
    part.row_count_ = row_count;
    part.merge_offset_ = merge_offset;
    part.schema_ = schema;
    return part;
}

// ---- Part::ReadColumn ----

Result<std::vector<Value>> Part::ReadColumn(int col_idx, ColumnType type) const {
    size_t eff_rows = effective_row_count();
    if (eff_rows == 0) return std::vector<Value>{};

    const auto& col_def = schema_.column_at(col_idx);
    std::string col_path = dir_ + "/" + col_def.name + ".col";

    auto cf = ColumnFile::Open(col_path, type);
    if (!cf.ok()) return cf.status;

    // 缺失列（ALTER TABLE ADD 后旧 Part）→ 返回有效行数个默认值
    if (cf->row_count() == 0 && row_count_ > 0) {
        std::vector<Value> out;
        out.reserve(eff_rows);
        if (type == ColumnType::FLOAT) {
            for (size_t i = 0; i < eff_rows; ++i) out.push_back(0.0);
        } else {
            for (size_t i = 0; i < eff_rows; ++i) out.push_back(int64_t(0));
        }
        return out;
    }

    // 读全量后跳过已 merge 的行
    if (type == ColumnType::FLOAT) {
        auto data = cf->ReadAllFloat64();
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(eff_rows);
        for (size_t i = merge_offset_; i < data->size(); ++i) out.push_back((*data)[i]);
        return out;
    } else {
        auto data = cf->ReadAllInt64();
        if (!data.ok()) return data.status;
        std::vector<Value> out;
        out.reserve(eff_rows);
        for (size_t i = merge_offset_; i < data->size(); ++i) out.push_back((*data)[i]);
        return out;
    }
}

// ---- Part::ReadColumnRange ----

Result<std::vector<Value>> Part::ReadColumnRange(int col_idx, ColumnType type, size_t start, size_t count) const {
    start += merge_offset_;  // 跳过已 merge 的行
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
        auto cf = ColumnFile::Open(tmp_path, type, /*exclusive=*/true);
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

// 不碰 .col 文件，仅递增 merge_offset_ + 更新 min_ts_ + 重写 meta.json。
Status Part::ConsumeRows(size_t n) {
    if (n == 0) return Status::OK();
    size_t new_offset = merge_offset_ + n;
    if (new_offset > row_count_)
        return Status(StatusCode::INVALID_ARGUMENT, "consume exceeds row_count");

    merge_offset_ = new_offset;

    // 更新 min_ts_（从 TS 列读取第一个有效行）
    for (size_t ci = 0; ci < schema_.column_count(); ++ci) {
        if (schema_.column_at(ci).type == ColumnType::TIMESTAMP) {
            const auto& col_def = schema_.column_at(ci);
            std::string col_path = dir_ + "/" + col_def.name + ".col";
            auto cf = ColumnFile::Open(col_path, col_def.type);
            if (cf.ok()) {
                if (merge_offset_ < row_count_) {
                    auto ts_data = cf->ReadRangeInt64(merge_offset_, 1);
                    if (ts_data.ok() && !ts_data->empty()) min_ts_ = (*ts_data)[0];
                }
                cf->Close();
            }
            break;
        }
    }

    TimePrecision ts_prec = TimePrecision::MICRO;
    for (size_t ci = 0; ci < schema_.column_count(); ++ci)
        if (schema_.column_at(ci).type == ColumnType::TIMESTAMP)
            { ts_prec = schema_.column_at(ci).precision; break; }

    std::string meta_path = dir_ + "/meta.json";
    return RewriteMetaJson(meta_path, min_ts_, max_ts_, row_count_, merge_offset_, merge_boundary_, ts_prec);
}

}  // namespace wavedb
