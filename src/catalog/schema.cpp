// Schema JSON 序列化/反序列化——使用 yyjson。

#include "wavedb/schema.h"

#include "third_party/yyjson.h"

namespace wavedb {

// ---- 类型名映射 ----

static const char* ColumnTypeName(ColumnType t) {
    switch (t) {
        case ColumnType::TIMESTAMP: return "TIMESTAMP";
        case ColumnType::FLOAT:     return "FLOAT";
        case ColumnType::INT:       return "INT";
    }
    return "UNKNOWN";
}

static ColumnType ColumnTypeFromName(const char* name) {
    if (!strcmp(name, "TIMESTAMP")) return ColumnType::TIMESTAMP;
    if (!strcmp(name, "FLOAT"))     return ColumnType::FLOAT;
    return ColumnType::INT;
}

// ---- ToJson ----

std::string TableSchema::ToJson() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "name", name_.c_str());

    // columns 数组
    yyjson_mut_val* cols_arr = yyjson_mut_arr(doc);
    for (auto& col : columns_) {
        yyjson_mut_val* col_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, col_obj, "name", col.name.c_str());
        yyjson_mut_obj_add_str(doc, col_obj, "type", ColumnTypeName(col.type));
        if (col.type == ColumnType::TIMESTAMP)
            yyjson_mut_obj_add_str(doc, col_obj, "precision", std::string(TimePrecisionName(col.precision)).c_str());
        yyjson_mut_arr_append(cols_arr, col_obj);
    }
    yyjson_mut_obj_add_val(doc, root, "columns", cols_arr);

    // merge 配置（仅非 NONE 时写入）
    if (merge_config_.policy != MergePolicy::NONE) {
        yyjson_mut_val* merge_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, merge_obj, "policy", std::string(MergePolicyName(merge_config_.policy)).c_str());
        if (merge_config_.merge_target_rows > 0)
            yyjson_mut_obj_add_int(doc, merge_obj, "merge_target_rows", merge_config_.merge_target_rows);
        if (merge_config_.use_compression)
            yyjson_mut_obj_add_bool(doc, merge_obj, "compress", true);
        yyjson_mut_obj_add_val(doc, root, "merge", merge_obj);
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json_str);
    free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

// ---- FromJson ----

static const char* GetStr(yyjson_val* obj, const char* key) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_str(v) : nullptr;
}

static int64_t GetInt(yyjson_val* obj, const char* key, int64_t def) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_sint(v) : def;
}

Result<TableSchema> TableSchema::FromJson(std::string_view json) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return Status(StatusCode::PARSE_ERROR, "invalid JSON in schema");

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return Status(StatusCode::PARSE_ERROR, "schema root is not object");
    }

    TableSchema schema;

    const char* name = GetStr(root, "name");
    if (!name) {
        yyjson_doc_free(doc);
        return Status(StatusCode::PARSE_ERROR, "table schema missing name");
    }
    schema.set_name(name);

    // columns
    yyjson_val* cols = yyjson_obj_get(root, "columns");
    if (cols && yyjson_is_arr(cols)) {
        size_t idx, max;
        yyjson_val* col_obj;
        yyjson_arr_foreach(cols, idx, max, col_obj) {
            if (!yyjson_is_obj(col_obj)) continue;
            const char* cname = GetStr(col_obj, "name");
            const char* ctype = GetStr(col_obj, "type");
            if (!cname || !ctype) continue;

            TimePrecision prec = TimePrecision::MICRO;
            const char* pstr = GetStr(col_obj, "precision");
            if (pstr) prec = TimePrecisionFromName(pstr);

            schema.AddColumn(cname, ColumnTypeFromName(ctype), prec);
        }
    }

    // merge
    yyjson_val* merge = yyjson_obj_get(root, "merge");
    if (merge && yyjson_is_obj(merge)) {
        const char* pstr = GetStr(merge, "policy");
        if (pstr) schema.merge_config_.policy = MergePolicyFromName(pstr);
        schema.merge_config_.merge_target_rows = GetInt(merge, "merge_target_rows", 0);
        yyjson_val* comp = yyjson_obj_get(merge, "compress");
        if (comp && yyjson_is_bool(comp))
            schema.merge_config_.use_compression = yyjson_get_bool(comp);
    }

    yyjson_doc_free(doc);
    return schema;
}

}  // namespace wavedb
