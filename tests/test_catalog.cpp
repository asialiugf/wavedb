#include <gtest/gtest.h>

#include <cstdio>

#include "src/catalog/catalog.h"
#include "wavedb/schema.h"

using namespace wavedb;

class CatalogTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/wavedb_catalog_test_XXXXXX";
        tmpdir_ = ::mkdtemp(tmpl);
    }
    void TearDown() override {
        {
            int rc = std::system(("rm -rf " + tmpdir_).c_str());
            (void)rc;
        };
    }
    std::string tmpdir_;
};

TEST_F(CatalogTest, SchemaJsonRoundtrip) {
    TableSchema schema("test_table");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::SECOND);
    schema.AddColumn("price", ColumnType::FLOAT);
    schema.AddColumn("volume", ColumnType::INT);

    std::string json = schema.ToJson();
    auto parsed = TableSchema::FromJson(json);
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed->name(), "test_table");
    EXPECT_EQ(parsed->column_count(), 3u);
    EXPECT_EQ(parsed->column_at(0).name, "ts");
    EXPECT_EQ(parsed->column_at(0).precision, TimePrecision::SECOND);
    EXPECT_EQ(parsed->RowByteSize(), 24u);
}

TEST_F(CatalogTest, ColumnDefLookup) {
    TableSchema schema("test");
    schema.AddColumn("ts", ColumnType::TIMESTAMP, TimePrecision::MICRO);
    schema.AddColumn("val", ColumnType::FLOAT);

    EXPECT_EQ(schema.ColumnIndex("ts"), 0);
    EXPECT_EQ(schema.ColumnIndex("val"), 1);
    EXPECT_EQ(schema.ColumnIndex("nope"), -1);
    EXPECT_NE(schema.FindColumn("ts"), nullptr);
    EXPECT_EQ(schema.FindColumn("nope"), nullptr);
}

TEST_F(CatalogTest, CreateTable) {
    auto cat = Catalog::Open(tmpdir_);
    ASSERT_TRUE(cat.ok());
    EXPECT_EQ(cat->table_count(), 0u);

    TableSchema schema("ticks");
    schema.AddColumn("ts", ColumnType::TIMESTAMP);

    ASSERT_TRUE(cat->CreateTable(schema).ok());
    EXPECT_EQ(cat->table_count(), 1u);
    EXPECT_NE(cat->GetTable("ticks"), nullptr);

    auto s = cat->CreateTable(schema);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::ALREADY_EXISTS);
}

TEST_F(CatalogTest, Restart) {
    {
        auto cat = Catalog::Open(tmpdir_);
        ASSERT_TRUE(cat.ok());
        TableSchema schema("ticks");
        schema.AddColumn("ts", ColumnType::TIMESTAMP);
        ASSERT_TRUE(cat->CreateTable(schema).ok());
    }
    {
        auto cat2 = Catalog::Open(tmpdir_);
        ASSERT_TRUE(cat2.ok());
        EXPECT_EQ(cat2->table_count(), 1u);
        EXPECT_NE(cat2->GetTable("ticks"), nullptr);
        EXPECT_EQ(cat2->GetTable("ticks")->column_count(), 1u);
    }
}

TEST_F(CatalogTest, TableNotFound) {
    auto cat = Catalog::Open(tmpdir_);
    ASSERT_TRUE(cat.ok());
    EXPECT_EQ(cat->GetTable("nonexistent"), nullptr);
}
