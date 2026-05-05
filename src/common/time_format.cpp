// 时间格式化与解析。
//
// 时间戳存储为微秒 epoch（int64_t），格式化为人类可读字符串。
// 格式统一为 UTC（gmtime_r / timegm），不受本地时区影响——
// 时序数据库通常使用 UTC 以避免夏令时/时区歧义。
//
// ParseTimestamp 容错设计：
//   输入可短于列精度，缺失部分自动补零。
//   例如列精度 MICRO 时，"20260101" 自动补全为
//   "20260101-00:00:00-000000"。
//   这允许用户在写入时使用低精度字面量。

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
    return TimePrecision::MICRO;  // 未知精度默认 MICRO
}

std::string FormatTimestamp(Timestamp ts, TimePrecision prec) {
    // 分离秒和亚秒部分，正确处理负时间戳
    int64_t sec = ts / 1'000'000;
    int64_t sub = ts % 1'000'000;
    if (sub < 0) {
        sec -= 1;
        sub += 1'000'000;
    }

    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);  // 线程安全版本

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
//
// 支持格式: YYYYMMDD[-HH[:MM[:SS[-sub]]]]
// col_prec 参数预留，v0.2 可用于校验输入精度不超过列精度。

static int Parse2Digits(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

static int Parse4Digits(const char* p) {
    return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
}

static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

Result<Timestamp> ParseTimestamp(std::string_view str, TimePrecision /*col_prec*/) {
    if (str.size() < 8) return Status(StatusCode::INVALID_ARGUMENT, "timestamp too short: " + std::string(str));

    auto p = str.data();
    auto end = p + str.size();

    // 日期部分：必须为 8 位数字
    for (int i = 0; i < 8; ++i)
        if (!IsDigit(p[i]))
            return Status(StatusCode::INVALID_ARGUMENT, "timestamp date must be 8 digits: " + std::string(str));

    int year = Parse4Digits(p);
    int month = Parse2Digits(p + 4);
    int day = Parse2Digits(p + 6);
    p += 8;

    int hour = 0, min = 0, sec = 0;
    int64_t sub = 0;

    // 可选时间部分: -HH[:MM[:SS[-sub]]]
    if (p < end && *p == '-') {
        ++p;
        if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
            return Status(StatusCode::INVALID_ARGUMENT, "expected HH after -: " + std::string(str));
        hour = Parse2Digits(p);
        p += 2;

        // :MM
        if (p < end && *p == ':') {
            ++p;
            if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
                return Status(StatusCode::INVALID_ARGUMENT, "expected MM after :: " + std::string(str));
            min = Parse2Digits(p);
            p += 2;

            // :SS
            if (p < end && *p == ':') {
                ++p;
                if (p + 2 > end || !IsDigit(p[0]) || !IsDigit(p[1]))
                    return Status(StatusCode::INVALID_ARGUMENT, "expected SS after :: " + std::string(str));
                sec = Parse2Digits(p);
                p += 2;
            }
        }

        // -sub（亚秒，1-6 位数字，自动右补零到微秒）
        if (p < end && *p == '-') {
            ++p;
            const char* sub_start = p;
            while (p < end && IsDigit(*p)) ++p;
            size_t sub_len = p - sub_start;
            if (sub_len == 0 || sub_len > 6)
                return Status(StatusCode::INVALID_ARGUMENT, "expected 1-6 sub-second digits: " + std::string(str));

            sub = 0;
            for (size_t i = 0; i < sub_len; ++i) sub = sub * 10 + (sub_start[i] - '0');
            // 右补零到微秒（例如 "123" -> 123000）
            for (size_t i = sub_len; i < 6; ++i) sub *= 10;
        }
    }

    // 不允许尾部非法字符
    if (p != end) return Status(StatusCode::INVALID_ARGUMENT, "unexpected trailing characters: " + std::string(str));

    // 基本范围校验（不校验日历合法性，如 2/30）
    if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31)
        return Status(StatusCode::INVALID_ARGUMENT, "date out of range: " + std::string(str));
    if (hour > 23 || min > 59 || sec > 59)
        return Status(StatusCode::INVALID_ARGUMENT, "time out of range: " + std::string(str));

    struct tm tm_buf = {};
    tm_buf.tm_year = year - 1900;
    tm_buf.tm_mon = month - 1;
    tm_buf.tm_mday = day;
    tm_buf.tm_hour = hour;
    tm_buf.tm_min = min;
    tm_buf.tm_sec = sec;
    tm_buf.tm_isdst = 0;  // 不使用 DST，统一 UTC

    time_t epoch = timegm(&tm_buf);  // UTC，不受 TZ 环境变量影响
    return static_cast<Timestamp>(epoch) * 1'000'000LL + sub;
}

// 将时间戳截断到指定精度（丢弃更细的部分）。
Timestamp TruncateToPrecision(Timestamp ts, TimePrecision prec) {
    constexpr int64_t kUsPerSec = 1'000'000LL;
    constexpr int64_t kUsPerMin = 60 * kUsPerSec;
    constexpr int64_t kUsPerHour = 3600 * kUsPerSec;
    constexpr int64_t kUsPerDay = 86400 * kUsPerSec;

    switch (prec) {
        case TimePrecision::DAY:    return (ts / kUsPerDay) * kUsPerDay;
        case TimePrecision::HOUR:   return (ts / kUsPerHour) * kUsPerHour;
        case TimePrecision::MINUTE: return (ts / kUsPerMin) * kUsPerMin;
        case TimePrecision::SECOND: return (ts / kUsPerSec) * kUsPerSec;
        case TimePrecision::MILLI:  return (ts / 1000) * 1000;
        case TimePrecision::MICRO:  return ts;
    }
    return ts;
}

// 将粗精度时间戳扩展到该周期的末尾（用于 <= 上界自适应）。
Timestamp ExpandToPeriodEnd(Timestamp ts, TimePrecision prec) {
    constexpr int64_t kUsPerSec = 1'000'000LL;
    constexpr int64_t kUsPerMin = 60 * kUsPerSec;
    constexpr int64_t kUsPerHour = 3600 * kUsPerSec;
    constexpr int64_t kUsPerDay = 86400 * kUsPerSec;

    switch (prec) {
        case TimePrecision::DAY:    return ts + kUsPerDay - 1;       // 23:59:59.999999
        case TimePrecision::HOUR:   return ts + kUsPerHour - 1;      // XX:59:59.999999
        case TimePrecision::MINUTE: return ts + kUsPerMin - 1;       // XX:XX:59.999999
        case TimePrecision::SECOND: return ts + kUsPerSec - 1;       // XX:XX:XX.999999
        case TimePrecision::MILLI:  return ts + 999;                 // XX:XX:XX.XXX999
        case TimePrecision::MICRO:  return ts;
    }
    return ts;
}

// 从时间戳字面量推断其精度。用于 WHERE 精度自适应。
TimePrecision TimestampLiteralPrecision(std::string_view s) {
    if (s.size() == 8) return TimePrecision::DAY;  // YYYYMMDD

    // 统计分隔符位置：date 后第一个 '-' 在 pos 8
    size_t first_colon = s.find(':');     // HH 后的 ':'
    size_t second_colon = s.npos;
    if (first_colon != s.npos) second_colon = s.find(':', first_colon + 1);
    size_t sub_dash = s.rfind('-');       // SS 后的 '-'（亚秒分隔）
    bool has_sub = (sub_dash != s.npos && sub_dash > 10);

    if (has_sub) {
        size_t sub_len = s.size() - sub_dash - 1;
        return sub_len <= 3 ? TimePrecision::MILLI : TimePrecision::MICRO;
    }
    if (second_colon != s.npos) return TimePrecision::SECOND;
    if (first_colon != s.npos) return TimePrecision::MINUTE;
    // 有 '-' 但无 ':' → HOUR
    if (s.size() > 8 && s[8] == '-') return TimePrecision::HOUR;
    return TimePrecision::MICRO;  // fallback
}

std::string_view MergePolicyName(MergePolicy policy) {
    switch (policy) {
        case MergePolicy::NONE:
            return "none";
        case MergePolicy::BY_HOUR:
            return "by_hour";
        case MergePolicy::BY_DAY:
            return "by_day";
        case MergePolicy::BY_MONTH:
            return "by_month";
    }
    return "none";
}

MergePolicy MergePolicyFromName(std::string_view name) {
    if (name == "by_hour") return MergePolicy::BY_HOUR;
    if (name == "by_day") return MergePolicy::BY_DAY;
    if (name == "by_month") return MergePolicy::BY_MONTH;
    return MergePolicy::NONE;  // 未知策略默认 NONE
}

}  // namespace wavedb
