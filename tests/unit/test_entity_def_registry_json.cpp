#include "entitydef/entity_def_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace atlas;

namespace
{

// Write a temporary entity_defs.json and return its path
auto write_temp_json(std::string_view content) -> std::filesystem::path
{
    auto tmp = std::filesystem::temp_directory_path() / "atlas_test_entity_defs.json";
    std::ofstream f(tmp);
    f << content;
    return tmp;
}

}  // namespace

// ============================================================================
// from_json_file — basic round-trip
// ============================================================================

TEST(EntityDefRegistryJson, LoadBasicTypes)
{
    auto path = write_temp_json(R"({
        "version": 1,
        "types": [
            {
                "type_id": 1,
                "name": "Account",
                "has_cell": false,
                "has_client": true,
                "properties": [
                    {"name": "accountName", "type": "string", "persistent": true,
                     "identifier": true, "scope": "base_only", "index": 0},
                    {"name": "level", "type": "int32", "persistent": true,
                     "identifier": false, "scope": "base_only", "index": 1}
                ]
            },
            {
                "type_id": 2,
                "name": "Avatar",
                "has_cell": true,
                "has_client": true,
                "properties": [
                    {"name": "hp", "type": "int32", "persistent": true,
                     "identifier": false, "scope": "own_client", "index": 0}
                ]
            }
        ]
    })");

    auto result = EntityDefRegistry::from_json_file(path);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto& reg = *result;
    EXPECT_EQ(reg.type_count(), 2u);

    auto* account = reg.find_by_name("Account");
    ASSERT_NE(account, nullptr);
    EXPECT_EQ(account->type_id, 1u);
    EXPECT_FALSE(account->has_cell);
    EXPECT_TRUE(account->has_client);
    EXPECT_EQ(account->properties.size(), 2u);

    EXPECT_EQ(account->properties[0].name, "accountName");
    EXPECT_TRUE(account->properties[0].persistent);
    EXPECT_TRUE(account->properties[0].identifier);
    EXPECT_EQ(account->properties[0].data_type, PropertyDataType::String);

    EXPECT_EQ(account->properties[1].name, "level");
    EXPECT_TRUE(account->properties[1].persistent);
    EXPECT_FALSE(account->properties[1].identifier);
    EXPECT_EQ(account->properties[1].data_type, PropertyDataType::Int32);

    auto* avatar = reg.find_by_id(2);
    ASSERT_NE(avatar, nullptr);
    EXPECT_EQ(avatar->name, "Avatar");
    EXPECT_TRUE(avatar->has_cell);
}

TEST(EntityDefRegistryJson, MissingTypesKeyReturnsError)
{
    auto path = write_temp_json(R"({"version":1})");
    auto result = EntityDefRegistry::from_json_file(path);
    EXPECT_FALSE(result.has_value());
}

TEST(EntityDefRegistryJson, NonExistentFileReturnsError)
{
    auto result = EntityDefRegistry::from_json_file("/nonexistent/path/entity_defs.json");
    EXPECT_FALSE(result.has_value());
}

TEST(EntityDefRegistryJson, SkipsInvalidEntries)
{
    // type_id=0 should be skipped
    auto path = write_temp_json(R"({
        "types": [
            {"type_id": 0, "name": "Bad", "properties": []},
            {"type_id": 3, "name": "Good", "properties": []}
        ]
    })");
    auto result = EntityDefRegistry::from_json_file(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type_count(), 1u);
    EXPECT_NE(result->find_by_name("Good"), nullptr);
}

// ============================================================================
// persistent_properties_digest — stability and sensitivity
// ============================================================================

TEST(EntityDefRegistryJson, DigestIsStable)
{
    auto path = write_temp_json(R"({
        "types": [
            {
                "type_id": 1, "name": "Account",
                "properties": [
                    {"name": "accountName", "type": "string", "persistent": true,
                     "identifier": true, "index": 0}
                ]
            }
        ]
    })");

    auto r1 = EntityDefRegistry::from_json_file(path);
    auto r2 = EntityDefRegistry::from_json_file(path);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    EXPECT_EQ(r1->persistent_properties_digest(), r2->persistent_properties_digest());
}

namespace
{
auto write_temp_json2(const char* suffix, std::string_view content) -> std::filesystem::path
{
    auto tmp = std::filesystem::temp_directory_path() /
               (std::string("atlas_test_entity_defs_") + suffix + ".json");
    std::ofstream f(tmp);
    f << content;
    return tmp;
}
}  // namespace

TEST(EntityDefRegistryJson, DigestChangesOnPropertyNameChange)
{
    auto path1 = write_temp_json2("prop_x", R"({
        "types": [{"type_id": 1, "name": "Foo",
            "properties": [{"name": "x", "type": "int32",
                            "persistent": true, "index": 0}]}]})");
    auto path2 = write_temp_json2("prop_y", R"({
        "types": [{"type_id": 1, "name": "Foo",
            "properties": [{"name": "y", "type": "int32",
                            "persistent": true, "index": 0}]}]})");

    auto r1 = EntityDefRegistry::from_json_file(path1);
    auto r2 = EntityDefRegistry::from_json_file(path2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(r1->persistent_properties_digest(), r2->persistent_properties_digest());
}

TEST(EntityDefRegistryJson, DigestIgnoresNonPersistentProperties)
{
    // Only non-persistent properties differ — digest should be the same
    auto path1 = write_temp_json2("transient_a", R"({
        "types": [{"type_id": 1, "name": "Foo",
            "properties": [{"name": "x", "type": "int32",
                            "persistent": true, "index": 0},
                           {"name": "transient_a", "type": "float",
                            "persistent": false, "index": 1}]}]})");
    auto path2 = write_temp_json2("transient_b", R"({
        "types": [{"type_id": 1, "name": "Foo",
            "properties": [{"name": "x", "type": "int32",
                            "persistent": true, "index": 0},
                           {"name": "transient_b", "type": "double",
                            "persistent": false, "index": 1}]}]})");

    auto r1 = EntityDefRegistry::from_json_file(path1);
    auto r2 = EntityDefRegistry::from_json_file(path2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->persistent_properties_digest(), r2->persistent_properties_digest());
}

// ============================================================================
// get_persistent_properties with identifier flag
// ============================================================================

TEST(EntityDefRegistryJson, GetPersistentPropertiesIncludesIdentifier)
{
    auto path = write_temp_json(R"({
        "types": [
            {
                "type_id": 1, "name": "Account",
                "properties": [
                    {"name": "accountName", "type": "string", "persistent": true,
                     "identifier": true, "index": 0},
                    {"name": "level", "type": "int32", "persistent": true,
                     "identifier": false, "index": 1},
                    {"name": "session", "type": "string", "persistent": false,
                     "identifier": false, "index": 2}
                ]
            }
        ]
    })");

    auto result = EntityDefRegistry::from_json_file(path);
    ASSERT_TRUE(result.has_value());

    auto props = result->get_persistent_properties(1);
    ASSERT_EQ(props.size(), 2u);

    // accountName should be marked as identifier
    const PropertyDescriptor* id_prop = nullptr;
    for (auto* p : props)
        if (p->identifier)
            id_prop = p;

    ASSERT_NE(id_prop, nullptr);
    EXPECT_EQ(id_prop->name, "accountName");
}
