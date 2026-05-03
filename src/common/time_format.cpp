#include <cstdio>
#include <ctime>

#include "wavedb/types.h"

namespace wavedb {

std::string_view TimePrecisionName(TimePrecision prec) {
    switch (prec) {
        case TimePrecision::DAY:
            return "DAY";
        case TimePrecision::HOUR:
            return "HOUR";
        case TimePrecision::MINUTE:
            return "MINUTE";
        case TimePrecision::SECOND:
            return "SECOND";
        case TimePrecision::MILLI:
            return "MILLI";
        case TimePrecision::MICRO:
            return "MICRO";
    }
    return "MICRO";
}

TimePrecision TimePrecisionFromName(std::string_view name) {
    if (name == "DAY") return TimePrecision::DAY;
    if (name == "HOUR") return TimePrecision::HOUR;
    if (name == "MINUTE") return TimePrecision::MINUTE;
    if (name == "SECOND") return TimePrecision::SECOND;
    if (name == "MILLI") return TimePrecision::MILLI;
    return TimePrecision::MICRO;
}

std::string FormatTimestamp(Timestamp ts, TimePrecision prec) {
    int64_t sec = ts / 1'000'000;
    int64_t sub = ts % 1'000'000;
    if (sub < 0) {
        sec -= 1;
        sub += 1'000'000;
    }

    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);

    char buf[48];
    switch (prec) {
        case TimePrecision::DAY:
            std::snprintf(buf, sizeof(buf), "%04d%02d%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
            break;
        case TimePrecision::HOUR:
            std::snprintf(
                buf, sizeof(buf), "%04d%02d%02d-%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour);
            break;
        case TimePrecision::MINUTE:
            std::snprintf(
                buf, sizeof(buf), "%04d%02d%02d-%02d:%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min);
            break;
        case TimePrecision::SECOND:
            std::snprintf(
                buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
            break;
        case TimePrecision::MILLI:
            std::snprintf(
                buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d-%03llu", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (unsigned long long)(sub / 1000));
            break;
        case TimePrecision::MICRO:
            std::snprintf(
                buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d-%06llu", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (unsigned long long)sub);
            break;
    }
    return buf;
}

// ---- ParseTimestamp ----

static int Parse2Digits(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

static int Parse4Digits(const char* p) {
    return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
}

static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

Result<Timestamp> ParseTimestamp(std::string_view str, TimePrecision /*col_prec*/) {
    // 格式: YYYYMMDD[-HH[:MM[:SS[-sub]]]]
    // 输入可短于列精度——缺失部分自动补零。
    // col_prec 预留，v0.2 可用于校验输入精度不超过列精度。

    if (str.size() < 8) return Status(StatusCode::kInvalidArgument, "timestamp too short: " + std::string(str));

    auto p = str.data();
    auto end = p + str.size();

    for (int i = 0; i < 8; ++i)
        if (!IsDigit(p[i]))
            return Status(StatusCode::kInvalidArgument, "timestamp date must be 8 digits: " + std::string(str));

    int year = Parse4Digits(p);
    int month = Parse2Digits(p + 4);
    int day = Parse2Digits(p + 6);
    p += 8;

    int hour = 0, min = 0, sec = 0;
    int64_t sub = 0;

    // -HH
    if (p < end && *p == '-') {
        ++p;
        if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
            return Status(StatusCode::kInvalidArgument, "expected HH after -: " + std::string(str));
        hour = Parse2Digits(p);
        p += 2;

        // :MM
        if (p < end && *p == ':') {
            ++p;
            if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
                return Status(StatusCode::kInvalidArgument, "expected MM after :: " + std::string(str));
            min = Parse2Digits(p);
            p += 2;

            // :SS
            if (p < end && *p == ':') {
                ++p;
                if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
                    return Status(StatusCode::kInvalidArgument, "expected SS after :: " + std::string(str));
                sec = Parse2Digits(p);
                p += 2;
            }
        }

        // -sub
        if (p < end && *p == '-') {
            ++p;
            const char* sub_start = p;
            while (p < end && IsDigit(*p)) ++p;
            size_t sub_len = p - sub_start;
            if (sub_len == 0 || sub_len > 6)
                return Status(StatusCode::kInvalidArgument, "expected 1-6 sub-second digits: " + std::string(str));

            sub = 0;
            for (size_t i = 0; i < sub_len; ++i) sub = sub * 10 + (sub_start[i] - '0');
            // 右补零到微秒
            for (size_t i = sub_len; i < 6; ++i) sub *= 10;
        }
    }

    // 剩余字符非法
    if (p != end) return Status(StatusCode::kInvalidArgument, "unexpected trailing characters: " + std::string(str));

    // 基础校验
    if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31)
        return Status(StatusCode::kInvalidArgument, "date out of range: " + std::string(str));
    if (hour > 23 || min > 59 || sec > 59)
        return Status(StatusCode::kInvalidArgument, "time out of range: " + std::string(str));

    struct tm tm_buf = {};
    tm_buf.tm_year = year - 1900;
    tm_buf.tm_mon = month - 1;
    tm_buf.tm_mday = day;
    tm_buf.tm_hour = hour;
    tm_buf.tm_min = min;
    tm_buf.tm_sec = sec;
    tm_buf.tm_isdst = 0;

    time_t epoch = timegm(&tm_buf);
    return static_cast<Timestamp>(epoch) * 1'000'000LL + sub;
}

}  // namespace wavedb
