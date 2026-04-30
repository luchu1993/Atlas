// Wire-format round-trip for ComponentDescriptor and the entity slot
// section appended to RegisterType.
//
// Two surfaces under test:
//   1. RegisterComponent(blob) — stand-alone component schema with
//      [u16 id][string name][string base_name][u8 locality][prop_count][props].
//   2. RegisterType(blob) trailing slot section —
//      [PackedInt slot_count][slot...] after the existing compression bytes.
//
// Slots reference components by component_type_id; the registry does NOT
// resolve slot↔component pointers eagerly because base components may be
// registered after derivatives during a batch startup.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

struct ComponentPropSpec {
  std::string_view name;
  PropertyDataType data_type;
  ReplicationScope scope = ReplicationScope::kAllClients;
  bool persistent = false;
  bool identifier = false;
  bool reliable = false;
  uint16_t index = 0;
};

auto MakeComponentBlob(uint16_t component_type_id, std::string_view name,
                       std::string_view base_name, ComponentLocality locality,
                       std::initializer_list<ComponentPropSpec> props) -> std::vector<std::byte> {
  BinaryWriter w;
  w.Write<uint16_t>(component_type_id);
  w.WriteString(name);
  w.WriteString(base_name);
  w.Write<uint8_t>(static_cast<uint8_t>(locality));
  w.WritePackedInt(static_cast<uint32_t>(props.size()));
  for (const auto& p : props) {
    w.WriteString(p.name);
    w.Write<uint8_t>(static_cast<uint8_t>(p.data_type));
    w.Write<uint8_t>(static_cast<uint8_t>(p.scope));
    w.Write<uint8_t>(p.persistent ? 1 : 0);
    w.Write<uint8_t>(5);  // detail_level
    w.Write<uint16_t>(p.index);
    w.Write<uint8_t>(p.identifier ? 1 : 0);
    w.Write<uint8_t>(p.reliable ? 1 : 0);
    // Component test only exercises scalars; container tail is covered by
    // test_entity_def_registry_container_prop, and the same helper drives
    // both code paths.
  }
  return w.Detach();
}

struct SlotSpec {
  uint16_t slot_idx;
  std::string_view slot_name;
  uint16_t component_type_id;
  ReplicationScope scope = ReplicationScope::kAllClients;
  bool lazy = false;
};

auto MakeTypeBlobWithSlots(std::string_view type_name, uint16_t type_id,
                           std::initializer_list<SlotSpec> slots) -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString(type_name);
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);  // 0 properties
  w.WritePackedInt(0);  // 0 rpcs

  // Slot section
  w.WritePackedInt(static_cast<uint32_t>(slots.size()));
  for (const auto& s : slots) {
    w.Write<uint16_t>(s.slot_idx);
    w.WriteString(s.slot_name);
    w.Write<uint16_t>(s.component_type_id);
    w.Write<uint8_t>(static_cast<uint8_t>(s.scope));
    w.Write<uint8_t>(s.lazy ? 1 : 0);
  }
  return w.Detach();
}

class RegistryComponentTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryComponentTest, RegisterComponentMinimal) {
  auto blob = MakeComponentBlob(1, "AbilityComponent", "", ComponentLocality::kSynced,
                                {ComponentPropSpec{.name = "cooldown",
                                                   .data_type = PropertyDataType::kFloat,
                                                   .scope = ReplicationScope::kAllClients}});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterComponent(blob.data(),
                                                              static_cast<int32_t>(blob.size())));

  const auto* c = EntityDefRegistry::Instance().FindComponentById(1);
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->name, "AbilityComponent");
  EXPECT_EQ(c->base_name, "");
  EXPECT_EQ(c->locality, ComponentLocality::kSynced);
  ASSERT_EQ(c->properties.size(), 1u);
  EXPECT_EQ(c->properties[0].name, "cooldown");
  EXPECT_EQ(c->properties[0].data_type, PropertyDataType::kFloat);
}

TEST_F(RegistryComponentTest, RegisterComponentWithInheritance) {
  // Base + leaf — verify base_name round-trips, even though the registry
  // does not (yet) resolve the chain pointer-wise.
  auto base = MakeComponentBlob(
      10, "AbilityComponent", "", ComponentLocality::kSynced,
      {ComponentPropSpec{.name = "cooldown", .data_type = PropertyDataType::kFloat}});
  auto derived = MakeComponentBlob(
      11, "AvatarAbility", "AbilityComponent", ComponentLocality::kSynced,
      {ComponentPropSpec{.name = "ranking", .data_type = PropertyDataType::kInt32}});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterComponent(base.data(),
                                                              static_cast<int32_t>(base.size())));
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterComponent(
      derived.data(), static_cast<int32_t>(derived.size())));

  const auto* d = EntityDefRegistry::Instance().FindComponentByName("AvatarAbility");
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->base_name, "AbilityComponent");
  ASSERT_EQ(d->properties.size(), 1u);
  EXPECT_EQ(d->properties[0].name, "ranking");

  // Base properties are NOT inlined into the leaf — runtime walks the chain.
  const auto* b = EntityDefRegistry::Instance().FindComponentByName("AbilityComponent");
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(b->properties.size(), 1u);
  EXPECT_EQ(b->properties[0].name, "cooldown");
}

TEST_F(RegistryComponentTest, RegisterComponentRejectsIdNameMismatch) {
  auto blob_a = MakeComponentBlob(20, "Foo", "", ComponentLocality::kSynced, {});
  auto blob_b = MakeComponentBlob(20, "Bar", "", ComponentLocality::kSynced, {});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterComponent(blob_a.data(),
                                                              static_cast<int32_t>(blob_a.size())));
  EXPECT_FALSE(EntityDefRegistry::Instance().RegisterComponent(blob_b.data(),
                                                               static_cast<int32_t>(blob_b.size())))
      << "Reusing id 20 with a different name must fail.";
}

TEST_F(RegistryComponentTest, RegisterComponentSameIdNameReplaces) {
  // Hot-reload path: same (id, name) pair, swapped properties.
  auto v1 = MakeComponentBlob(
      30, "Mover", "", ComponentLocality::kServerLocal,
      {ComponentPropSpec{.name = "speed", .data_type = PropertyDataType::kFloat}});
  auto v2 = MakeComponentBlob(
      30, "Mover", "", ComponentLocality::kServerLocal,
      {ComponentPropSpec{.name = "speed", .data_type = PropertyDataType::kFloat},
       ComponentPropSpec{.name = "accel", .data_type = PropertyDataType::kFloat, .index = 1}});
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterComponent(v1.data(), static_cast<int32_t>(v1.size())));
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterComponent(v2.data(), static_cast<int32_t>(v2.size())));

  const auto* c = EntityDefRegistry::Instance().FindComponentByName("Mover");
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->properties.size(), 2u);
}

TEST_F(RegistryComponentTest, EntitySlotSectionRoundTrip) {
  // Slot table appended after the (otherwise property-/rpc-empty) entity
  // body and the compression bytes.
  auto blob = MakeTypeBlobWithSlots("AvatarWithAbility", 100,
                                    {SlotSpec{.slot_idx = 1,
                                              .slot_name = "ability",
                                              .component_type_id = 11,
                                              .scope = ReplicationScope::kOwnClient,
                                              .lazy = false},
                                     SlotSpec{.slot_idx = 2,
                                              .slot_name = "mover",
                                              .component_type_id = 30,
                                              .scope = ReplicationScope::kCellPrivate,
                                              .lazy = true}});
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("AvatarWithAbility");
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->slots.size(), 2u);

  EXPECT_EQ(d->slots[0].slot_idx, 1);
  EXPECT_EQ(d->slots[0].slot_name, "ability");
  EXPECT_EQ(d->slots[0].component_type_id, 11);
  EXPECT_EQ(d->slots[0].scope, ReplicationScope::kOwnClient);
  EXPECT_FALSE(d->slots[0].lazy);

  EXPECT_EQ(d->slots[1].slot_idx, 2);
  EXPECT_EQ(d->slots[1].slot_name, "mover");
  EXPECT_EQ(d->slots[1].component_type_id, 30);
  EXPECT_EQ(d->slots[1].scope, ReplicationScope::kCellPrivate);
  EXPECT_TRUE(d->slots[1].lazy);
}

TEST_F(RegistryComponentTest, EntityWithoutSlotSectionStillWorks) {
  // Older blobs that omit the slot section entirely must still register —
  // the section reader runs only when there are remaining bytes.
  BinaryWriter w;
  w.WriteString("Plain");
  w.Write<uint16_t>(200);
  w.Write<uint8_t>(1);  // has_cell
  w.Write<uint8_t>(1);  // has_client
  w.WritePackedInt(0);  // 0 properties
  w.WritePackedInt(0);  // 0 rpcs
  // No slot section — the trailing-bytes branch is not entered.
  auto blob = w.Detach();

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
  const auto* d = EntityDefRegistry::Instance().FindByName("Plain");
  ASSERT_NE(d, nullptr);
  EXPECT_TRUE(d->slots.empty());
}

TEST_F(RegistryComponentTest, ClearResetsComponents) {
  auto blob = MakeComponentBlob(42, "Tmp", "", ComponentLocality::kSynced, {});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterComponent(blob.data(),
                                                              static_cast<int32_t>(blob.size())));
  ASSERT_NE(EntityDefRegistry::Instance().FindComponentById(42), nullptr);

  EntityDefRegistry::Instance().clear();
  EXPECT_EQ(EntityDefRegistry::Instance().FindComponentById(42), nullptr);
  EXPECT_EQ(EntityDefRegistry::Instance().ComponentCount(), 0u);
}

}  // namespace atlas
