#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "serialization/data_section.h"

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

TEST(DataSection, FromXmlStringReadValues) {
  auto result = DataSection::FromXmlString(kTestXml);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  EXPECT_EQ(root->ReadString("name"), "TestServer");
  EXPECT_EQ(root->ReadInt("port"), 8080);
  EXPECT_NEAR(root->ReadFloat("rate"), 1.5f, 1e-5f);
  EXPECT_TRUE(root->ReadBool("enabled"));
}

TEST(DataSection, FromJsonStringReadValues) {
  auto result = DataSection::FromJsonString(kTestJson);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  EXPECT_EQ(root->ReadString("name"), "TestServer");
  EXPECT_EQ(root->ReadInt("port"), 8080);
  EXPECT_NEAR(root->ReadFloat("rate"), 1.5f, 1e-5f);
  EXPECT_TRUE(root->ReadBool("enabled"));
}

TEST(DataSection, ChildNavigation) {
  auto result = DataSection::FromXmlString(kTestXml);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  auto db = root->Child("database");
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(db->ReadString("host"), "localhost");
  EXPECT_EQ(db->ReadInt("port"), 3306);
}

TEST(DataSection, ChildrenList) {
  auto result = DataSection::FromXmlString(kTestXml);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  auto& kids = root->Children();
  EXPECT_GE(kids.size(), 5u);
}

TEST(DataSection, MissingKeyReturnsDefault) {
  auto result = DataSection::FromXmlString(kTestXml);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  EXPECT_EQ(root->ReadString("nonexistent", "fallback"), "fallback");
  EXPECT_EQ(root->ReadInt("nonexistent", -1), -1);
  EXPECT_NEAR(root->ReadFloat("nonexistent", 9.9f), 9.9f, 1e-5f);
  EXPECT_FALSE(root->ReadBool("nonexistent", false));
}

TEST(DataSection, MalformedXmlReturnsError) {
  auto result = DataSection::FromXmlString("<root><broken");
  EXPECT_FALSE(result.HasValue());
}

TEST(DataSection, MalformedJsonReturnsError) {
  auto result = DataSection::FromJsonString("{invalid json!!!");
  EXPECT_FALSE(result.HasValue());
}

TEST(DataSection, NestedChildrenFromJson) {
  auto result = DataSection::FromJsonString(kTestJson);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  auto db = root->Child("database");
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(db->ReadString("host"), "localhost");
  EXPECT_EQ(db->ReadInt("port"), 3306);
}

// ============================================================================
// XML attribute support (stored as @name child nodes)
// ============================================================================

TEST(DataSection, XmlAttributesParsedWithAtPrefix) {
  const char* xml = R"(
<entity type="npc" id="42">
    <name>Goblin</name>
</entity>
)";
  auto result = DataSection::FromXmlString(xml);
  ASSERT_TRUE(result.HasValue());
  auto root = *result;

  EXPECT_EQ(root->ReadString("@type"), "npc");
  EXPECT_EQ(root->ReadInt("@id"), 42);
  EXPECT_EQ(root->ReadString("name"), "Goblin");
}

TEST(DataSection, XmlChildAttributesParsed) {
  const char* xml = R"(
<root>
    <weapon damage="15" range="2"/>
</root>
)";
  auto result = DataSection::FromXmlString(xml);
  ASSERT_TRUE(result.HasValue());
  auto weapon = (*result)->Child("weapon");
  ASSERT_NE(weapon, nullptr);
  EXPECT_EQ(weapon->ReadInt("@damage"), 15);
  EXPECT_EQ(weapon->ReadInt("@range"), 2);
}

TEST(DataSection, XmlNoAttributesDoesNotAddAtChildren) {
  const char* xml = R"(<root><item>value</item></root>)";
  auto result = DataSection::FromXmlString(xml);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ((*result)->ReadString("@type", "none"), "none");
}

// ============================================================================
// File loading (from_xml / from_json)
// ============================================================================

class DataSectionFileTest : public ::testing::Test {
 protected:
  std::filesystem::path xml_path_;
  std::filesystem::path json_path_;

  void SetUp() override {
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

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(xml_path_, ec);
    std::filesystem::remove(json_path_, ec);
  }
};

TEST_F(DataSectionFileTest, FromXmlFile) {
  auto result = DataSection::FromXml(xml_path_);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_EQ((*result)->ReadString("host"), "myserver");
  EXPECT_EQ((*result)->ReadInt("port"), 9000);
}

TEST_F(DataSectionFileTest, FromJsonFile) {
  auto result = DataSection::FromJson(json_path_);
  ASSERT_TRUE(result.HasValue()) << result.Error().Message();
  EXPECT_EQ((*result)->ReadString("host"), "myserver");
  EXPECT_EQ((*result)->ReadInt("port"), 9000);
}

TEST_F(DataSectionFileTest, FromXmlNonExistentReturnsError) {
  auto result = DataSection::FromXml("/no/such/file_atlas_xyz.xml");
  EXPECT_FALSE(result.HasValue());
}

TEST_F(DataSectionFileTest, FromJsonNonExistentReturnsError) {
  auto result = DataSection::FromJson("/no/such/file_atlas_xyz.json");
  EXPECT_FALSE(result.HasValue());
}

// ============================================================================
// JSON arrays: stored as children with numeric string keys ("0", "1", ...)
// ============================================================================

TEST(DataSection, JsonArrayChildrenHaveNumericKeys) {
  const char* json = R"({"items":["alpha","beta","gamma"]})";
  auto result = DataSection::FromJsonString(json);
  ASSERT_TRUE(result.HasValue());

  auto items = (*result)->Child("items");
  ASSERT_NE(items, nullptr);

  EXPECT_EQ(items->ReadString("0"), "alpha");
  EXPECT_EQ(items->ReadString("1"), "beta");
  EXPECT_EQ(items->ReadString("2"), "gamma");
}

TEST(DataSection, JsonArrayOfObjects) {
  const char* json = R"({"servers":[{"host":"a","port":1},{"host":"b","port":2}]})";
  auto result = DataSection::FromJsonString(json);
  ASSERT_TRUE(result.HasValue());

  auto servers = (*result)->Child("servers");
  ASSERT_NE(servers, nullptr);

  auto s0 = servers->Child("0");
  ASSERT_NE(s0, nullptr);
  EXPECT_EQ(s0->ReadString("host"), "a");
  EXPECT_EQ(s0->ReadInt("port"), 1);

  auto s1 = servers->Child("1");
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1->ReadString("host"), "b");
  EXPECT_EQ(s1->ReadInt("port"), 2);
}

TEST(DataSection, JsonEmptyArray) {
  const char* json = R"({"items":[]})";
  auto result = DataSection::FromJsonString(json);
  ASSERT_TRUE(result.HasValue());

  auto items = (*result)->Child("items");
  ASSERT_NE(items, nullptr);
  EXPECT_TRUE(items->Children().empty());
}

// ============================================================================
// Deep nesting
// ============================================================================

TEST(DataSection, XmlDeepNesting) {
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
  auto result = DataSection::FromXmlString(xml);
  ASSERT_TRUE(result.HasValue());

  auto l1 = (*result)->Child("level1");
  ASSERT_NE(l1, nullptr);
  auto l2 = l1->Child("level2");
  ASSERT_NE(l2, nullptr);
  auto l3 = l2->Child("level3");
  ASSERT_NE(l3, nullptr);
  auto l4 = l3->Child("level4");
  ASSERT_NE(l4, nullptr);
  EXPECT_EQ(l4->ReadString("level5"), "deep_value");
}

TEST(DataSection, JsonDeepNesting) {
  const char* json = R"({"a":{"b":{"c":{"d":{"e":42}}}}})";
  auto result = DataSection::FromJsonString(json);
  ASSERT_TRUE(result.HasValue());

  auto a = (*result)->Child("a");
  ASSERT_NE(a, nullptr);
  auto b = a->Child("b");
  ASSERT_NE(b, nullptr);
  auto c = b->Child("c");
  ASSERT_NE(c, nullptr);
  auto d = c->Child("d");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->ReadInt("e"), 42);
}

// ============================================================================
// children(name) — multiple siblings with same key
// ============================================================================

TEST(DataSection, ChildrenByNameReturnsAll) {
  const char* xml = R"(
<root>
  <item>one</item>
  <item>two</item>
  <item>three</item>
</root>
)";
  auto result = DataSection::FromXmlString(xml);
  ASSERT_TRUE(result.HasValue());

  auto items = (*result)->Children("item");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0]->Value(), "one");
  EXPECT_EQ(items[1]->Value(), "two");
  EXPECT_EQ(items[2]->Value(), "three");
}

// ============================================================================
// Manual construction
// ============================================================================

TEST(DataSection, ManualBuildAndRead) {
  auto tree = std::make_shared<DataSectionTree>("root");
  tree->AddChild("name", "Atlas");
  tree->AddChild("version", "1");
  auto* sub = tree->AddChild("network");
  sub->AddChild("port", "7000");

  EXPECT_EQ(tree->ReadString("name"), "Atlas");
  EXPECT_EQ(tree->ReadInt("version"), 1);

  auto* net = tree->Child("network");
  ASSERT_NE(net, nullptr);
  EXPECT_EQ(net->ReadInt("port"), 7000);
}
