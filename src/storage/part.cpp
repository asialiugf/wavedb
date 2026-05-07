// Part 实现：n_ 开头的不可变数据分区的创建、打开、读取、删除、截断。
//
// Part 目录结构（n_YYYYMMDD_XXXXXX 格式）：
//   n_20260101_000001/
//     meta.json  → 时间范围 + 行数
//     ts.col     → TIMESTAMP 列数据（裸 int64_t 数组）
//     price.col  → FLOAT 列数据（裸 double 数组）
//     volume.col → INT 列数据（裸 int64_t 数组）
//
// n_ 序号持久化在 <parts_dir>/.n_seq 文件中（格式 "日期 序号"），
// 同天自增，跨天从 1 开始。
//
// meta.json 包含 min_ts / max_ts 供 PartManager 时间裁剪使用。
// 合并部分消费后通过 DiscardFirstRows 重写 .col + 更新 meta.json。

#include "src/storage/part.h"

#include <sys/stat.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>

#include "src/storage/column_file.h"
#include "third_party/yyjson.h"
#include "wavedb/schema.h"

namespace wavedb {

// ---- n_ 命名工具 ----

int Part::TsToDate(int64_t ts_us) {
    time_t sec = static_cast<time_t>(ts_us / 1'000'000);
    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);
    return (tm_buf.tm_year + 1900) * 10000 + (tm_buf.tm_mon + 1) * 100 + tm_buf.tm_mday;
}

std::string Part::MakePartDir(const std::string& parts_dir, int date, int seq) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "n_%08d_%06d", date, seq);
    return parts_dir + "/" + buf;
}

int Part::NextSeq(const std::string& parts_dir, int date) {
    std::string seq_path = parts_dir + "/.n_seq";
    int last_seq = 0;
    int last_date = 0;
    std::ifstream ifs(seq_path);
    if (ifs.is_open()) {
        ifs >> last_date >> last_seq;
        ifs.close();
    }
    int next_seq = (date == last_date) ? last_seq + 1 : 1;
    std::ofstream ofs(seq_path, std::ios::trunc);
    if (ofs.is_open()) {
        ofs << date << " " << next_seq;
        ofs.close();
    }
    return next_seq;
}

// ---- meta.json 读写 ----

static char* BuildMetaJson(
    int64_t min_ts, int64_t max_ts, size_t row_count,
    int64_t merge_boundary, bool in_progress, TimePrecision prec) {
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
    if (merge_boundary != 0)
        yyjson_mut_obj_add_int(doc, root, "merge_boundary", merge_boundary);
    if (in_progress)
        yyjson_mut_obj_add_str(doc, root, "status", "in_progress");

    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    yyjson_mut_doc_free(doc);
    return json;
}

static Status WriteMetaJson(
    const std::string& path, int64_t min_ts, int64_t max_ts, size_t row_count,
    int64_t merge_boundary, bool in_progress, TimePrecision prec) {
    char* json = BuildMetaJson(min_ts, max_ts, row_count, merge_boundary, in_progress, prec);
    if (!json) return Status(StatusCode::INTERNAL, "yyjson build failed");

    // .tmp + rename 保证原子性：Reader 要么看到旧文件，要么看到完整新文件
    std::string tmp_path = path + ".tmp";
    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (!f) { free(json); return Status(StatusCode::IO_ERROR, "cannot write: " + tmp_path); }
    size_t len = strlen(json);
    size_t n = std::fwrite(json, 1, len, f);
    std::fclose(f);
    free(json);
    if (n != len) return Status(StatusCode::IO_ERROR, "write truncated: " + tmp_path);
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0)
        return Status(StatusCode::IO_ERROR, "rename failed: " + tmp_path);
    return Status::OK();
}

// ---- Part::CreateImpl ----

Result<Part> Part::CreateImpl(
    std::string part_dir,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns,
    int64_t min_ts,
    int64_t max_ts) {
    if (::mkdir(part_dir.c_str(), 0755) != 0) return Status(StatusCode::IO_ERROR, "mkdir failed: " + part_dir);

    size_t ncols = schema.column_count();
    if (columns.size() != ncols) return Status(StatusCode::INVALID_ARGUMENT, "column count mismatch");

    size_t nrows = columns.empty() ? 0 : columns[0].size();

    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = schema.column_at(ci);
        std::string col_path = part_dir + "/" + col_def.name + ".col";

        auto cf = ColumnFile::Open(col_path, col_def.type, /*exclusive=*/true);
        if (!cf.ok()) return cf.status;

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

    TimePrecision ts_prec = TimePrecision::MICRO;
    for (size_t ci = 0; ci < ncols; ++ci)
        if (schema.column_at(ci).type == ColumnType::TIMESTAMP) { ts_prec = schema.column_at(ci).precision; break; }

    std::string meta_path = part_dir + "/meta.json";
    Status s = WriteMetaJson(meta_path, min_ts, max_ts, nrows, 0, false, ts_prec);
    if (!s.ok()) return s;

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
    part.row_count_ = nrows;
    part.schema_ = schema;
    return part;
}

// ---- Part::Create / CreateWithPath ----

Result<Part> Part::Create(
    std::string parts_dir,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns,
    int64_t min_ts,
    int64_t max_ts) {
    ::mkdir(parts_dir.c_str(), 0755);

    int date = TsToDate(min_ts);
    int seq = NextSeq(parts_dir, date);
    std::string part_dir = MakePartDir(parts_dir, date, seq);
    return CreateImpl(std::move(part_dir), schema, columns, min_ts, max_ts);
}

Result<Part> Part::CreateWithPath(
    std::string part_dir,
    const TableSchema& schema,
    const std::vector<std::vector<Value>>& columns,
    int64_t min_ts,
    int64_t max_ts) {
    return CreateImpl(std::move(part_dir), schema, columns, min_ts, max_ts);
}

// ---- Part::Open ----

Result<Part> Part::Open(std::string part_dir, const TableSchema& schema) {
    std::string meta_path = part_dir + "/meta.json";

    // rename(.tmp, meta.json) 写入瞬间 Reader 可能读不到 → 重试 3 次
    yyjson_doc* doc = nullptr;
    for (int attempt = 0; attempt < 3 && !doc; ++attempt) {
        doc = yyjson_read_file(meta_path.c_str(), YYJSON_READ_ALLOW_COMMENTS, nullptr, nullptr);
        if (!doc && attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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

    // merge_offset 字段兼容读取旧格式，读取后忽略（row_count 已是实际行数）
    // 若旧 meta.json 有 merge_offset，则实际有效行数 = row_count - merge_offset
    yyjson_val* v_off = yyjson_obj_get(root, "merge_offset");
    if (v_off) {
        size_t old_offset = static_cast<size_t>(yyjson_get_sint(v_off));
        row_count -= old_offset;
    }

    int64_t merge_boundary = 0;
    yyjson_val* v_bnd = yyjson_obj_get(root, "merge_boundary");
    if (v_bnd) merge_boundary = yyjson_get_sint(v_bnd);

    bool in_progress = false;
    yyjson_val* v_status = yyjson_obj_get(root, "status");
    if (v_status) {
        const char* st = yyjson_get_str(v_status);
        if (st && strcmp(st, "in_progress") == 0) in_progress = true;
    }

    yyjson_doc_free(doc);

    Part part;
    part.dir_ = std::move(part_dir);
    part.min_ts_ = min_ts;
    part.max_ts_ = max_ts;
    part.merge_boundary_ = merge_boundary;
    part.in_progress_ = in_progress;
    part.row_count_ = row_count;
    part.schema_ = schema;
    return part;
}

// ---- Part::ReadColumn ----

Result<std::vector<Value>> Part::ReadColumn(int col_idx, ColumnType type) const {
    if (row_count_ == 0) return std::vector<Value>{};

    const auto& col_def = schema_.column_at(col_idx);
    std::string col_path = dir_ + "/" + col_def.name + ".col";

    auto cf = ColumnFile::Open(col_path, type);
    if (!cf.ok()) return std::vector<Value>{};  // 文件被 merge 删 → 返回空，数据已在 m_ 中

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
    if (!cf.ok()) return std::vector<Value>{};  // 文件被 merge 删 → 返回空

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

Status Part::WriteColumn(std::string_view col_name, ColumnType type, const std::vector<Value>& values) const {
    if (values.size() != row_count_)
        return Status(
            StatusCode::INVALID_ARGUMENT,
            "values count mismatch: " + std::to_string(values.size()) + " vs " + std::to_string(row_count_));

    std::string col_path = dir_ + "/" + std::string(col_name) + ".col";
    std::string tmp_path = col_path + ".tmp";

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

    if (std::rename(tmp_path.c_str(), col_path.c_str()) != 0)
        return Status(StatusCode::IO_ERROR, "rename failed: " + tmp_path);

    return Status::OK();
}

// ---- Part::DiscardFirstRows ----
// 读取每列 [n, row_count_) 的行 → 写 .col.tmp → rename → 更新 row_count_ + meta.json。
Status Part::DiscardFirstRows(size_t n) {
    if (n == 0) return Status::OK();
    if (n >= row_count_)
        return Status(StatusCode::INVALID_ARGUMENT, "discard " + std::to_string(n) + " >= row_count " + std::to_string(row_count_));

    size_t keep = row_count_ - n;
    size_t ncols = schema_.column_count();

    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = schema_.column_at(ci);
        std::string col_path = dir_ + "/" + col_def.name + ".col";
        std::string tmp_path = col_path + ".tmp";

        auto cf = ColumnFile::Open(col_path, col_def.type);
        if (!cf.ok()) return cf.status;

        // 读到 temp 再写新文件
        if (col_def.type == ColumnType::FLOAT) {
            auto data = cf->ReadRangeFloat64(n, keep);
            cf->Close();
            if (!data.ok()) return data.status;
            auto tmp = ColumnFile::Open(tmp_path, col_def.type, /*exclusive=*/true);
            if (!tmp.ok()) return tmp.status;
            Status s = tmp->Append(std::span<const double>(*data));
            if (!s.ok()) return s;
            tmp->Close();
        } else {
            auto data = cf->ReadRangeInt64(n, keep);
            cf->Close();
            if (!data.ok()) return data.status;
            auto tmp = ColumnFile::Open(tmp_path, col_def.type, /*exclusive=*/true);
            if (!tmp.ok()) return tmp.status;
            Status s = tmp->Append(std::span<const int64_t>(*data));
            if (!s.ok()) return s;
            tmp->Close();
        }

        if (std::rename(tmp_path.c_str(), col_path.c_str()) != 0)
            return Status(StatusCode::IO_ERROR, "rename failed: " + tmp_path);
    }

    row_count_ = keep;

    // 更新 min_ts_（从 TS 列读取新第一行），重置 merge_boundary 待下次重算
    merge_boundary_ = 0;
    for (size_t ci = 0; ci < ncols; ++ci) {
        if (schema_.column_at(ci).type == ColumnType::TIMESTAMP) {
            std::string col_path = dir_ + "/" + schema_.column_at(ci).name + ".col";
            auto cf = ColumnFile::Open(col_path, schema_.column_at(ci).type);
            if (cf.ok()) {
                auto ts_data = cf->ReadRangeInt64(0, 1);
                if (ts_data.ok() && !ts_data->empty()) min_ts_ = (*ts_data)[0];
                cf->Close();
            }
            break;
        }
    }

    TimePrecision ts_prec = TimePrecision::MICRO;
    for (size_t ci = 0; ci < ncols; ++ci)
        if (schema_.column_at(ci).type == ColumnType::TIMESTAMP)
            { ts_prec = schema_.column_at(ci).precision; break; }

    std::string meta_path = dir_ + "/meta.json";
    return WriteMetaJson(meta_path, min_ts_, max_ts_, row_count_, merge_boundary_, in_progress_, ts_prec);
}

Status Part::PersistMeta() const {
    TimePrecision ts_prec = TimePrecision::MICRO;
    for (size_t ci = 0; ci < schema_.column_count(); ++ci)
        if (schema_.column_at(ci).type == ColumnType::TIMESTAMP)
            { ts_prec = schema_.column_at(ci).precision; break; }
    std::string meta_path = dir_ + "/meta.json";
    return WriteMetaJson(meta_path, min_ts_, max_ts_, row_count_, merge_boundary_, in_progress_, ts_prec);
}

Status Part::AppendColumns(const std::vector<std::vector<Value>>& columns) {
    size_t ncols = schema_.column_count();
    if (columns.size() != ncols) return Status(StatusCode::INVALID_ARGUMENT, "column count mismatch");
    size_t nrows = columns.empty() ? 0 : columns[0].size();
    if (nrows == 0) return Status::OK();

    for (size_t ci = 0; ci < ncols; ++ci) {
        const auto& col_def = schema_.column_at(ci);
        std::string col_path = dir_ + "/" + col_def.name + ".col";
        auto cf = ColumnFile::Open(col_path, col_def.type);
        if (!cf.ok()) return cf.status;
        if (col_def.type == ColumnType::FLOAT) {
            std::vector<double> buf; buf.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) buf.push_back(std::get<double>(columns[ci][r]));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        } else {
            std::vector<int64_t> buf; buf.reserve(nrows);
            for (size_t r = 0; r < nrows; ++r) buf.push_back(std::get<int64_t>(columns[ci][r]));
            Status s = cf->Append(buf);
            if (!s.ok()) return s;
        }
        cf->Close();
    }

    // 更新 TS 范围
    if (nrows > 0) {
        for (size_t ci = 0; ci < ncols; ++ci) {
            if (schema_.column_at(ci).type == ColumnType::TIMESTAMP) {
                int64_t first_ts = std::get<int64_t>(columns[ci].front());
                int64_t last_ts  = std::get<int64_t>(columns[ci].back());
                if (first_ts < min_ts_ || row_count_ == 0) min_ts_ = first_ts;
                if (last_ts > max_ts_) max_ts_ = last_ts;
                break;
            }
        }
    }
    row_count_ += nrows;
    return PersistMeta();
}

// ---- Part::Delete ----

Status Part::Delete() const {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    if (ec) return Status(StatusCode::IO_ERROR, "remove_all failed: " + dir_);
    return Status::OK();
}

}  // namespace wavedb
