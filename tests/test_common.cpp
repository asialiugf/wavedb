#include <gtest/gtest.h>

#include <string>

#include "wavedb/status.h"
#include "wavedb/types.h"

using namespace wavedb;

TEST(StatusTest, Ok) {
    Status ok = Status::OK();
    EXPECT_TRUE(ok.ok());
    EXPECT_EQ(ok.code(), StatusCode::OK);
}

TEST(StatusTest, Error) {
    Status err(StatusCode::NOT_FOUND, "table not found");
    EXPECT_FALSE(err.ok());
    EXPECT_EQ(err.code(), StatusCode::NOT_FOUND);
    EXPECT_EQ(err.message(), "table not found");
}

TEST(StatusTest, DefaultIsOk) {
    Status s;
    EXPECT_TRUE(s.ok());
}

TEST(ResultTest, Value) {
    Result<int> r(42);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(*r, 42);
}

TEST(ResultTest, Error) {
    Result<int> r(Status(StatusCode::IO_ERROR, "disk full"));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.status.code(), StatusCode::IO_ERROR);
}

TEST(ColumnTypeSizeTest, All) {
    EXPECT_EQ(ColumnTypeSize(ColumnType::TIMESTAMP), 8);
    EXPECT_EQ(ColumnTypeSize(ColumnType::FLOAT), 8);
    EXPECT_EQ(ColumnTypeSize(ColumnType::INT), 8);
}

TEST(FormatTimestampTest, AllPrecisions) {
    // 2026-01-01 10:50:00 UTC = 1767264600000000 us
    Timestamp ts = 1767264600000000LL;
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::DAY), "20260101");
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::HOUR), "20260101-10");
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::MINUTE), "20260101-10:50");
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::SECOND), "20260101-10:50:00");
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::MILLI), "20260101-10:50:00-000");
    EXPECT_EQ(FormatTimestamp(ts, TimePrecision::MICRO), "20260101-10:50:00-000000");
}

TEST(ParseTimestampTest, Day) {
    auto r = ParseTimestamp("20260101", TimePrecision::MICRO);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(FormatTimestamp(*r, TimePrecision::MICRO), "20260101-00:00:00-000000");
}

TEST(ParseTimestampTest, Minute) {
    auto r = ParseTimestamp("20260101-09:30", TimePrecision::MICRO);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(FormatTimestamp(*r, TimePrecision::MINUTE), "20260101-09:30");
}

TEST(ParseTimestampTest, Micro) {
    auto r = ParseTimestamp("20260101-09:30:00-123456", TimePrecision::MICRO);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(FormatTimestamp(*r, TimePrecision::MICRO), "20260101-09:30:00-123456");
}

TEST(ParseTimestampTest, ShortInputAutoComplete) {
    auto r = ParseTimestamp("20260101", TimePrecision::SECOND);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(FormatTimestamp(*r, TimePrecision::MICRO), "20260101-00:00:00-000000");
}

TEST(ParseTimestampTest, EmptyFails) {
    auto r = ParseTimestamp("", TimePrecision::SECOND);
    EXPECT_FALSE(r.ok());
}

TEST(TimePrecisionTest, NameMapping) {
    EXPECT_EQ(TimePrecisionName(TimePrecision::DAY), "DAY");
    EXPECT_EQ(TimePrecisionName(TimePrecision::MICRO), "MICRO");
    EXPECT_EQ(TimePrecisionFromName("SECOND"), TimePrecision::SECOND);
    EXPECT_EQ(TimePrecisionFromName("HOUR"), TimePrecision::HOUR);
}

TEST(ValueTest, Int64) {
    Value v = int64_t(42);
    EXPECT_TRUE(std::holds_alternative<int64_t>(v));
    EXPECT_EQ(std::get<int64_t>(v), 42);
}

TEST(ValueTest, Double) {
    Value v = 3.14;
    EXPECT_TRUE(std::holds_alternative<double>(v));
    EXPECT_EQ(std::get<double>(v), 3.14);
}
