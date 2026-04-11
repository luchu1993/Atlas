#include "serialization/data_section.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

// ============================================================================
// XML attribute support (stored as @name child nodes)
// ============================================================================

TEST(DataSection, XmlAttributesParsedWithAtPrefix)
{
    const char* xml = R"(
<entity type="npc" id="42">
    <name>Goblin</name>
</entity>
)";
    auto result = DataSection::from_xml_string(xml);
    ASSERT_TRUE(result.has_value());
    auto root = *result;

    EXPECT_EQ(root->read_string("@type"), "npc");
    EXPECT_EQ(root->read_int("@id"), 42);
    EXPECT_EQ(root->read_string("name"), "Goblin");
}

TEST(DataSection, XmlChildAttributesParsed)
{
    const char* xml = R"(
<root>
    <weapon damage="15" range="2"/>
</root>
)";
    auto result = DataSection::from_xml_string(xml);
    ASSERT_TRUE(result.has_value());
    auto weapon = (*result)->child("weapon");
    ASSERT_NE(weapon, nullptr);
    EXPECT_EQ(weapon->read_int("@damage"), 15);
    EXPECT_EQ(weapon->read_int("@range"), 2);
}

TEST(DataSection, XmlNoAttributesDoesNotAddAtChildren)
{
    const char* xml = R"(<root><item>value</item></root>)";
    auto result = DataSection::from_xml_string(xml);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)->read_string("@type", "none"), "none");
}

// ============================================================================
// File loading (from_xml / from_json)
// ============================================================================

class DataSectionFileTest : public ::testing::Test
{
protected:
    std::filesystem::path xml_path_;
    std::filesystem::path json_path_;

    void SetUp() override
    {
        auto tmp = std::filesystem::temp_directory_path();
        xml_path_ = tmp / "atlas_test_ds.xml";
        json_path_ = tmp / "atlas_test_ds.json";

        // Write a small XML file
        {
            std::ofstream f(xml_path_);
            f << R"(<config><host>myserver</host><port>9000</port></config>)";
        }
        // Write a small JSON file
        {
            std::ofstream f(json_path_);
            f << R"({"host":"myserver","port":9000})";
        }
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(xml_path_, ec);
        std::filesystem::remove(json_path_, ec);
    }
};

TEST_F(DataSectionFileTest, FromXmlFile)
{
    auto result = DataSection::from_xml(xml_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ((*result)->read_string("host"), "myserver");
    EXPECT_EQ((*result)->read_int("port"), 9000);
}

TEST_F(DataSectionFileTest, FromJsonFile)
{
    auto result = DataSection::from_json(json_path_);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ((*result)->read_string("host"), "myserver");
    EXPECT_EQ((*result)->read_int("port"), 9000);
}

TEST_F(DataSectionFileTest, FromXmlNonExistentReturnsError)
{
    auto result = DataSection::from_xml("/no/such/file_atlas_xyz.xml");
    EXPECT_FALSE(result.has_value());
}

TEST_F(DataSectionFileTest, FromJsonNonExistentReturnsError)
{
    auto result = DataSection::from_json("/no/such/file_atlas_xyz.json");
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// JSON arrays: stored as children with numeric string keys ("0", "1", ...)
// ============================================================================

TEST(DataSection, JsonArrayChildrenHaveNumericKeys)
{
    const char* json = R"({"items":["alpha","beta","gamma"]})";
    auto result = DataSection::from_json_string(json);
    ASSERT_TRUE(result.has_value());

    auto items = (*result)->child("items");
    ASSERT_NE(items, nullptr);

    EXPECT_EQ(items->read_string("0"), "alpha");
    EXPECT_EQ(items->read_string("1"), "beta");
    EXPECT_EQ(items->read_string("2"), "gamma");
}

TEST(DataSection, JsonArrayOfObjects)
{
    const char* json = R"({"servers":[{"host":"a","port":1},{"host":"b","port":2}]})";
    auto result = DataSection::from_json_string(json);
    ASSERT_TRUE(result.has_value());

    auto servers = (*result)->child("servers");
    ASSERT_NE(servers, nullptr);

    auto s0 = servers->child("0");
    ASSERT_NE(s0, nullptr);
    EXPECT_EQ(s0->read_string("host"), "a");
    EXPECT_EQ(s0->read_int("port"), 1);

    auto s1 = servers->child("1");
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(s1->read_string("host"), "b");
    EXPECT_EQ(s1->read_int("port"), 2);
}

TEST(DataSection, JsonEmptyArray)
{
    const char* json = R"({"items":[]})";
    auto result = DataSection::from_json_string(json);
    ASSERT_TRUE(result.has_value());

    auto items = (*result)->child("items");
    ASSERT_NE(items, nullptr);
    EXPECT_TRUE(items->children().empty());
}

// ============================================================================
// Deep nesting
// ============================================================================

TEST(DataSection, XmlDeepNesting)
{
    const char* xml = R"(
<root>
  <level1>
    <level2>
      <level3>
        <level4>
          <level5>deep_value</level5>
        </level4>
      </level3>
    </level2>
  </level1>
</root>
)";
    auto result = DataSection::from_xml_string(xml);
    ASSERT_TRUE(result.has_value());

    auto l1 = (*result)->child("level1");
    ASSERT_NE(l1, nullptr);
    auto l2 = l1->child("level2");
    ASSERT_NE(l2, nullptr);
    auto l3 = l2->child("level3");
    ASSERT_NE(l3, nullptr);
    auto l4 = l3->child("level4");
    ASSERT_NE(l4, nullptr);
    EXPECT_EQ(l4->read_string("level5"), "deep_value");
}

TEST(DataSection, JsonDeepNesting)
{
    const char* json = R"({"a":{"b":{"c":{"d":{"e":42}}}}})";
    auto result = DataSection::from_json_string(json);
    ASSERT_TRUE(result.has_value());

    auto a = (*result)->child("a");
    ASSERT_NE(a, nullptr);
    auto b = a->child("b");
    ASSERT_NE(b, nullptr);
    auto c = b->child("c");
    ASSERT_NE(c, nullptr);
    auto d = c->child("d");
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->read_int("e"), 42);
}

// ============================================================================
// children(name) — multiple siblings with same key
// ============================================================================

TEST(DataSection, ChildrenByNameReturnsAll)
{
    const char* xml = R"(
<root>
  <item>one</item>
  <item>two</item>
  <item>three</item>
</root>
)";
    auto result = DataSection::from_xml_string(xml);
    ASSERT_TRUE(result.has_value());

    auto items = (*result)->children("item");
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0]->value(), "one");
    EXPECT_EQ(items[1]->value(), "two");
    EXPECT_EQ(items[2]->value(), "three");
}

// ============================================================================
// Manual construction
// ============================================================================

TEST(DataSection, ManualBuildAndRead)
{
    auto tree = std::make_shared<DataSectionTree>("root");
    tree->add_child("name", "Atlas");
    tree->add_child("version", "1");
    auto* sub = tree->add_child("network");
    sub->add_child("port", "7000");

    EXPECT_EQ(tree->read_string("name"), "Atlas");
    EXPECT_EQ(tree->read_int("version"), 1);

    auto* net = tree->child("network");
    ASSERT_NE(net, nullptr);
    EXPECT_EQ(net->read_int("port"), 7000);
}
