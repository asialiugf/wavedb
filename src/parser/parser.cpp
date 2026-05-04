// SQL 解析器实现：Tokenizer + 递归下降解析。
//
// Tokenizer 按空白和标点切分，关键字大小写不敏感。
// Parser 直接调用 ParseCallbacks 回调，无中间 AST。

#include "src/parser/parser.h"

#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

namespace wavedb {

// ---- Tokenizer ----

enum class TokenKind {
    KW_CREATE,
    KW_TABLE,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_AND,
    KW_LIMIT,
    KW_UPDATE,
    KW_SET,
    KW_TO,
    KW_ALTER,
    KW_ADD,
    KW_DROP,
    KW_COLUMN,
    KW_FIELD,  // COLUMN 的同义词
    KW_TIMESTAMP,
    KW_FLOAT,
    KW_INT,
    KW_DAY,
    KW_HOUR,
    KW_MINUTE,
    KW_SECOND,
    KW_MILLI,
    KW_MICRO,
    KW_MONTH,
    KW_MERGE,
    KW_MAX_ROWS,
    KW_BY,
    IDENT,
    NUMBER,
    STRING,
    TIMESTAMP_LITERAL,
    STAR,
    COMMA,
    LPAREN,
    RPAREN,
    SEMI,
    GTE,  // >=
    LTE,  // <=
    EQ,   // =
    END,
    ERR,
};

struct Token {
    TokenKind kind = TokenKind::END;
    std::string_view text;
};

// 关键字映射表
static const std::pair<std::string_view, TokenKind> kKeywords[] = {
    {"create", TokenKind::KW_CREATE},
    {"table", TokenKind::KW_TABLE},
    {"insert", TokenKind::KW_INSERT},
    {"into", TokenKind::KW_INTO},
    {"values", TokenKind::KW_VALUES},
    {"select", TokenKind::KW_SELECT},
    {"from", TokenKind::KW_FROM},
    {"where", TokenKind::KW_WHERE},
    {"and", TokenKind::KW_AND},
    {"limit", TokenKind::KW_LIMIT},
    {"update", TokenKind::KW_UPDATE},
    {"set", TokenKind::KW_SET},
    {"to", TokenKind::KW_TO},
    {"alter", TokenKind::KW_ALTER},
    {"add", TokenKind::KW_ADD},
    {"drop", TokenKind::KW_DROP},
    {"column", TokenKind::KW_COLUMN},
    {"field", TokenKind::KW_FIELD},
    {"timestamp", TokenKind::KW_TIMESTAMP},
    {"float", TokenKind::KW_FLOAT},
    {"int", TokenKind::KW_INT},
    {"day", TokenKind::KW_DAY},
    {"hour", TokenKind::KW_HOUR},
    {"minute", TokenKind::KW_MINUTE},
    {"second", TokenKind::KW_SECOND},
    {"milli", TokenKind::KW_MILLI},
    {"micro", TokenKind::KW_MICRO},
    {"month", TokenKind::KW_MONTH},
    {"merge", TokenKind::KW_MERGE},
    {"max_rows", TokenKind::KW_MAX_ROWS},
    {"by", TokenKind::KW_BY},
};

class Tokenizer {
  public:
    explicit Tokenizer(std::string_view input) : p_(input.data()), end_(p_ + input.size()) {}

    Token Next() {
        SkipWS();
        if (p_ >= end_) return {TokenKind::END, {}};
        char c = *p_;

        // 标点
        if (c == ',') {
            ++p_;
            return {TokenKind::COMMA, {}};
        }
        if (c == '(') {
            ++p_;
            return {TokenKind::LPAREN, {}};
        }
        if (c == ')') {
            ++p_;
            return {TokenKind::RPAREN, {}};
        }
        if (c == ';') {
            ++p_;
            return {TokenKind::SEMI, {}};
        }
        if (c == '*') {
            ++p_;
            return {TokenKind::STAR, {}};
        }

        // >= <= =
        if (c == '>' && p_ + 1 < end_ && p_[1] == '=') {
            p_ += 2;
            return {TokenKind::GTE, {}};
        }
        if (c == '<' && p_ + 1 < end_ && p_[1] == '=') {
            p_ += 2;
            return {TokenKind::LTE, {}};
        }
        if (c == '=') {
            ++p_;
            return {TokenKind::EQ, {}};
        }

        // 字符串字面量 (timestamp)
        if (IsTimestampLiteralStart()) return ReadTimestamp();
        // 数字
        if (std::isdigit(c) || c == '-' || c == '+') return ReadNumber();
        // 标识符 / 关键字
        return ReadIdent();
    }

  private:
    const char* p_;
    const char* end_;

    void SkipWS() {
        while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }

    bool IsTimestampLiteralStart() const {
        // 8 位日期 + 可选时间
        if (p_ + 8 > end_) return false;
        for (int i = 0; i < 8; ++i)
            if (!std::isdigit(static_cast<unsigned char>(p_[i]))) return false;
        // 后面紧跟 - 或空白或标点才是 timestamp literal
        return true;
    }

    Token ReadTimestamp() {
        const char* start = p_;
        // YYYYMMDD
        p_ += 8;
        // -HH[:MM[:SS[-sub]]]
        if (p_ < end_ && *p_ == '-') {
            ++p_;
            if (p_ + 1 < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
                p_ += 2;  // HH
                if (p_ < end_ && *p_ == ':') {
                    ++p_;
                    if (p_ + 1 < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
                        p_ += 2;  // MM
                        if (p_ < end_ && *p_ == ':') {
                            ++p_;
                            if (p_ + 1 < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
                                p_ += 2;  // SS
                                if (p_ < end_ && *p_ == '-') {
                                    ++p_;
                                    while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
                                }
                            }
                        }
                    }
                }
            }
        }
        return {TokenKind::TIMESTAMP_LITERAL, std::string_view(start, static_cast<size_t>(p_ - start))};
    }

    Token ReadNumber() {
        const char* start = p_;
        if (*p_ == '-' || *p_ == '+') ++p_;
        while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
        // 浮点
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) ++p_;
        }
        return {TokenKind::NUMBER, std::string_view(start, static_cast<size_t>(p_ - start))};
    }

    Token ReadIdent() {
        const char* start = p_;
        while (p_ < end_ && !std::isspace(static_cast<unsigned char>(*p_)) && *p_ != ',' && *p_ != '(' && *p_ != ')' &&
               *p_ != ';' && *p_ != '=' && *p_ != '<' && *p_ != '>')
            ++p_;
        std::string_view word(start, static_cast<size_t>(p_ - start));
        // 关键字匹配（大小写不敏感）
        for (auto& kw : kKeywords) {
            if (word.size() == kw.first.size()) {
                bool match = true;
                for (size_t i = 0; i < word.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(word[i])) != static_cast<unsigned char>(kw.first[i])) {
                        match = false;
                        break;
                    }
                }
                if (match) return {kw.second, word};
            }
        }
        return {TokenKind::IDENT, word};
    }
};

// ---- Parser ----

class Parser {
  public:
    Parser(std::string_view sql, const ParseCallbacks& cb) : tz_(sql), cb_(cb) { Advance(); }

    Status Parse() {
        if (tok_.kind == TokenKind::END || tok_.kind == TokenKind::SEMI) return Status::OK();  // 空语句

        TokenKind kw = tok_.kind;
        if (kw == TokenKind::KW_CREATE) return ParseCreate();
        if (kw == TokenKind::KW_INSERT) return ParseInsert();
        if (kw == TokenKind::KW_SELECT) return ParseSelect();
        if (kw == TokenKind::KW_ALTER) return ParseAlter();
        if (kw == TokenKind::KW_UPDATE) return ParseUpdate();

        return Status(StatusCode::PARSE_ERROR, std::string("unexpected: ") + std::string(tok_.text));
    }

  private:
    Tokenizer tz_;
    const ParseCallbacks& cb_;
    Token tok_;

    void Advance() { tok_ = tz_.Next(); }

    Status Expect(TokenKind k, const char* msg) {
        if (tok_.kind != k)
            return Status(StatusCode::PARSE_ERROR, std::string(msg) + ", got: " + std::string(tok_.text));
        Advance();
        return Status::OK();
    }

    // CREATE TABLE name (col TYPE [prec], ...)
    Status ParseCreate() {
        Advance();  // skip CREATE
        auto s = Expect(TokenKind::KW_TABLE, "expected TABLE");
        if (!s.ok()) return s;

        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected table name");
        std::string table_name(tok_.text);
        Advance();

        s = Expect(TokenKind::LPAREN, "expected (");
        if (!s.ok()) return s;

        std::vector<std::string> col_names;
        std::vector<ColumnType> col_types;
        std::vector<TimePrecision> col_precs;

        while (tok_.kind != TokenKind::RPAREN && tok_.kind != TokenKind::END) {
            if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected column name");
            col_names.emplace_back(tok_.text);
            Advance();

            auto [type, prec] = ParseColumnType();
            col_types.push_back(type);
            col_precs.push_back(prec);

            if (tok_.kind == TokenKind::COMMA) Advance();
        }
        s = Expect(TokenKind::RPAREN, "expected )");
        if (!s.ok()) return s;

        if (col_names.empty()) return Status(StatusCode::PARSE_ERROR, "at least one column required");

        // 解析可选的 MERGE BY policy [MAX_ROWS n]
        MergeConfig merge_cfg;
        if (tok_.kind == TokenKind::KW_MERGE) {
            Advance();  // skip MERGE
            s = Expect(TokenKind::KW_BY, "expected BY");
            if (!s.ok()) return s;

            if (tok_.kind == TokenKind::KW_HOUR)
                merge_cfg.policy = MergePolicy::BY_HOUR;
            else if (tok_.kind == TokenKind::KW_DAY)
                merge_cfg.policy = MergePolicy::BY_DAY;
            else if (tok_.kind == TokenKind::KW_MONTH)
                merge_cfg.policy = MergePolicy::BY_MONTH;
            else
                return Status(StatusCode::PARSE_ERROR, "expected DAY, HOUR, or MONTH");
            Advance();

            if (tok_.kind == TokenKind::KW_MAX_ROWS) {
                Advance();
                auto val = ParseValue();
                if (val && std::holds_alternative<int64_t>(*val))
                    merge_cfg.max_rows_per_part = std::get<int64_t>(*val);
            }
        }

        return cb_.on_create_table(table_name, col_names, col_types, col_precs, merge_cfg);
    }

    std::pair<ColumnType, TimePrecision> ParseColumnType() {
        TokenKind k = tok_.kind;
        if (k == TokenKind::KW_TIMESTAMP) {
            Advance();
            TimePrecision prec = TimePrecision::MICRO;
            if (tok_.kind == TokenKind::LPAREN) {
                Advance();
                prec = ParsePrecision();
                Expect(TokenKind::RPAREN, "expected )");
            }
            return {ColumnType::TIMESTAMP, prec};
        }
        if (k == TokenKind::KW_FLOAT) {
            Advance();
            return {ColumnType::FLOAT, TimePrecision::MICRO};
        }
        if (k == TokenKind::KW_INT) {
            Advance();
            return {ColumnType::INT, TimePrecision::MICRO};
        }
        // 默认 INT
        Advance();
        return {ColumnType::INT, TimePrecision::MICRO};
    }

    TimePrecision ParsePrecision() {
        TokenKind k = tok_.kind;
        Advance();
        switch (k) {
            case TokenKind::KW_DAY:
                return TimePrecision::DAY;
            case TokenKind::KW_HOUR:
                return TimePrecision::HOUR;
            case TokenKind::KW_MINUTE:
                return TimePrecision::MINUTE;
            case TokenKind::KW_SECOND:
                return TimePrecision::SECOND;
            case TokenKind::KW_MILLI:
                return TimePrecision::MILLI;
            default:
                return TimePrecision::MICRO;
        }
    }

    // INSERT INTO name VALUES (val, ...)
    Status ParseInsert() {
        Advance();  // skip INSERT
        auto s = Expect(TokenKind::KW_INTO, "expected INTO");
        if (!s.ok()) return s;

        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected table name");
        std::string table_name(tok_.text);
        Advance();

        s = Expect(TokenKind::KW_VALUES, "expected VALUES");
        if (!s.ok()) return s;
        s = Expect(TokenKind::LPAREN, "expected (");
        if (!s.ok()) return s;

        std::vector<Value> values;
        while (tok_.kind != TokenKind::RPAREN && tok_.kind != TokenKind::END) {
            auto val = ParseValue();
            if (!val) return Status(StatusCode::PARSE_ERROR, "expected value");
            values.push_back(*val);
            if (tok_.kind == TokenKind::COMMA) Advance();
        }
        s = Expect(TokenKind::RPAREN, "expected )");
        if (!s.ok()) return s;

        return cb_.on_insert(table_name, values);
    }

    std::optional<Value> ParseValue() {
        if (tok_.kind == TokenKind::NUMBER) {
            std::string_view text = tok_.text;
            Advance();
            // 包含 '.' 则为浮点
            if (text.find('.') != std::string_view::npos) {
                double d = 0;
                auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
                if (ec == std::errc{}) return d;
                return std::nullopt;
            }
            int64_t i = 0;
            auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), i);
            if (ec == std::errc{}) return i;
            return std::nullopt;
        }
        if (tok_.kind == TokenKind::TIMESTAMP_LITERAL) {
            std::string_view text = tok_.text;
            Advance();
            auto r = ParseTimestamp(text, TimePrecision::MICRO);
            if (r.ok()) return int64_t(*r);
            return std::nullopt;
        }
        return std::nullopt;
    }

    // SELECT [*|col,...] FROM name [WHERE ts >= val [AND ts <= val]] [LIMIT n]
    Status ParseSelect() {
        Advance();  // skip SELECT

        std::vector<std::string> cols;
        if (tok_.kind == TokenKind::STAR) {
            Advance();
        } else {
            while (tok_.kind != TokenKind::KW_FROM && tok_.kind != TokenKind::END) {
                if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected column name");
                cols.emplace_back(tok_.text);
                Advance();
                if (tok_.kind == TokenKind::COMMA) Advance();
            }
        }

        auto s = Expect(TokenKind::KW_FROM, "expected FROM");
        if (!s.ok()) return s;

        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected table name");
        std::string table_name(tok_.text);
        Advance();

        Timestamp from_ts = 0, to_ts = 0;
        int64_t limit = 0;

        // WHERE ts >= val [AND ts <= val]
        if (tok_.kind == TokenKind::KW_WHERE) {
            Advance();
            // 期望: column >= val [AND column <= val]
            if (tok_.kind == TokenKind::IDENT) Advance();  // skip column name
            if (tok_.kind == TokenKind::GTE || tok_.kind == TokenKind::EQ) {
                Advance();
                auto val = ParseValue();
                if (val && std::holds_alternative<int64_t>(*val)) from_ts = std::get<int64_t>(*val);
            }
            if (tok_.kind == TokenKind::KW_AND) {
                Advance();
                if (tok_.kind == TokenKind::IDENT) Advance();  // skip column name
                if (tok_.kind == TokenKind::LTE || tok_.kind == TokenKind::EQ) {
                    Advance();
                    auto val = ParseValue();
                    if (val && std::holds_alternative<int64_t>(*val)) to_ts = std::get<int64_t>(*val);
                }
            }
        }

        if (tok_.kind == TokenKind::KW_LIMIT) {
            Advance();
            auto val = ParseValue();
            if (val && std::holds_alternative<int64_t>(*val)) limit = std::get<int64_t>(*val);
        }

        std::vector<std::string> out_col_names;
        std::vector<ColumnType> out_col_types;
        std::vector<TimePrecision> out_col_precs;
        std::vector<std::vector<Value>> out_rows;

        s = cb_.on_select(
            table_name, cols, from_ts, to_ts, limit, out_col_names, out_col_types, out_col_precs, out_rows);
        return s;
    }

    // ALTER TABLE name ADD COLUMN name TYPE
    // ALTER TABLE name DROP COLUMN name
    Status ParseAlter() {
        Advance();  // skip ALTER
        auto s = Expect(TokenKind::KW_TABLE, "expected TABLE");
        if (!s.ok()) return s;

        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected table name");
        std::string table_name(tok_.text);
        Advance();

        if (tok_.kind == TokenKind::KW_ADD) {
            Advance();
            // 接受 COLUMN 或 FIELD（同义词）
            if (tok_.kind != TokenKind::KW_COLUMN && tok_.kind != TokenKind::KW_FIELD)
                return Status(StatusCode::PARSE_ERROR, "expected COLUMN or FIELD");
            Advance();
            if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected column name");
            std::string col_name(tok_.text);
            Advance();
            auto [type, prec] = ParseColumnType();
            return cb_.on_add_column(table_name, col_name, type, prec);
        }

        if (tok_.kind == TokenKind::KW_DROP) {
            Advance();
            // 接受 COLUMN 或 FIELD（同义词）
            if (tok_.kind != TokenKind::KW_COLUMN && tok_.kind != TokenKind::KW_FIELD)
                return Status(StatusCode::PARSE_ERROR, "expected COLUMN or FIELD");
            Advance();
            if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected column name");
            std::string col_name(tok_.text);
            Advance();
            return cb_.on_drop_column(table_name, col_name);
        }

        return Status(StatusCode::PARSE_ERROR, "expected ADD or DROP after ALTER TABLE");
    }

    // UPDATE table SET col = val,... [FROM ts TO ts]
    Status ParseUpdate() {
        Advance();  // skip UPDATE
        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected table name");
        std::string table_name(tok_.text);
        Advance();

        auto s = Expect(TokenKind::KW_SET, "expected SET");
        if (!s.ok()) return s;

        if (tok_.kind != TokenKind::IDENT) return Status(StatusCode::PARSE_ERROR, "expected column name");
        std::string col_name(tok_.text);
        Advance();

        s = Expect(TokenKind::EQ, "expected =");
        if (!s.ok()) return s;

        // 逗号分隔的值列表
        std::vector<Value> values;
        while (tok_.kind != TokenKind::KW_FROM && tok_.kind != TokenKind::END && tok_.kind != TokenKind::SEMI) {
            auto val = ParseValue();
            if (!val) return Status(StatusCode::PARSE_ERROR, "expected value");
            values.push_back(*val);
            if (tok_.kind == TokenKind::COMMA) Advance();
        }

        // FROM/TO 可选：指定则按范围更新（from_ts/to_ts 传给回调），不指定则全表更新（from_ts=0/to_ts=0）
        Timestamp from_ts = 0, to_ts = 0;
        if (tok_.kind == TokenKind::KW_FROM) {
            Advance();
            auto from_val = ParseValue();
            if (!from_val || !std::holds_alternative<int64_t>(*from_val))
                return Status(StatusCode::PARSE_ERROR, "expected from timestamp");
            from_ts = std::get<int64_t>(*from_val);

            s = Expect(TokenKind::KW_TO, "expected TO");
            if (!s.ok()) return s;

            auto to_val = ParseValue();
            if (!to_val || !std::holds_alternative<int64_t>(*to_val))
                return Status(StatusCode::PARSE_ERROR, "expected to timestamp");
            to_ts = std::get<int64_t>(*to_val);
        }

        return cb_.on_update_column(table_name, col_name, from_ts, to_ts, values);
    }
};

// ---- 公开接口 ----

Status ParseSQL(std::string_view sql, const ParseCallbacks& cb) {
    Parser parser(sql, cb);
    return parser.Parse();
}

}  // namespace wavedb
