#include <gtest/gtest.h>
#include "serialization/data_section.hpp"

#include <string>

using namespace atlas;

static const char* kTestXml = R"(
<root>
    <name>TestServer</name>
    <port>8080</port>
    <rate>1.5</rate>
    <enabled>true</enabled>
    <database>
        <host>localhost</host>
        <port>3306</port>
    </database>
</root>
)";

static const char* kTestJson = R"({
    "name": "TestServer",
    "port": 8080,
    "rate": 1.5,
    "enabled": true,
    "database": {
        "host": "localhost",
        "port": 3306
    }
})";

TEST(DataSection, FromXmlStringReadValues)
{
    auto result = DataSection::from_xml_string(kTestXml);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    EXPECT_EQ(root->read_string("name"), "TestServer");
    EXPECT_EQ(root->read_int("port"), 8080);
    EXPECT_NEAR(root->read_float("rate"), 1.5f, 1e-5f);
    EXPECT_TRUE(root->read_bool("enabled"));
}

TEST(DataSection, FromJsonStringReadValues)
{
    auto result = DataSection::from_json_string(kTestJson);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    EXPECT_EQ(root->read_string("name"), "TestServer");
    EXPECT_EQ(root->read_int("port"), 8080);
    EXPECT_NEAR(root->read_float("rate"), 1.5f, 1e-5f);
    EXPECT_TRUE(root->read_bool("enabled"));
}

TEST(DataSection, ChildNavigation)
{
    auto result = DataSection::from_xml_string(kTestXml);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    auto db = root->child("database");
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->read_string("host"), "localhost");
    EXPECT_EQ(db->read_int("port"), 3306);
}

TEST(DataSection, ChildrenList)
{
    auto result = DataSection::from_xml_string(kTestXml);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    auto& kids = root->children();
    EXPECT_GE(kids.size(), 5u);
}

TEST(DataSection, MissingKeyReturnsDefault)
{
    auto result = DataSection::from_xml_string(kTestXml);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    EXPECT_EQ(root->read_string("nonexistent", "fallback"), "fallback");
    EXPECT_EQ(root->read_int("nonexistent", -1), -1);
    EXPECT_NEAR(root->read_float("nonexistent", 9.9f), 9.9f, 1e-5f);
    EXPECT_FALSE(root->read_bool("nonexistent", false));
}

TEST(DataSection, MalformedXmlReturnsError)
{
    auto result = DataSection::from_xml_string("<root><broken");
    EXPECT_FALSE(result.has_value());
}

TEST(DataSection, MalformedJsonReturnsError)
{
    auto result = DataSection::from_json_string("{invalid json!!!");
    EXPECT_FALSE(result.has_value());
}

TEST(DataSection, NestedChildrenFromJson)
{
    auto result = DataSection::from_json_string(kTestJson);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    auto db = root->child("database");
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(db->read_string("host"), "localhost");
    EXPECT_EQ(db->read_int("port"), 3306);
}
