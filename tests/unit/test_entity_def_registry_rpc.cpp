// Tests for RpcDescriptor direction/exposed metadata via the binary
// descriptor path (EntityDefRegistry::RegisterType).
//
// Phase 10 Step 10.9 relies on RpcDescriptor::Direction() and
// RpcDescriptor::exposed being populated correctly so the BaseApp can:
//   - reject client→Base RPCs whose direction isn't 0x03
//   - reject client→Cell RPCs whose direction isn't 0x02
//   - reject non-exposed RPCs (exposed == kNone)
//   - enforce OwnClient vs AllClients cross-entity rules
//
// These tests synthesize a minimal binary descriptor matching the wire format
// emitted by Atlas.Generators.Def.TypeRegistryEmitter, feed it through
// RegisterType, and assert the registry returns the expected fields.
//
// Direction is encoded in the packed rpc_id (bits 23:22). RpcIdEmitter uses:
//   - client_methods → 0x00
//   - cell_methods   → 0x02
//   - base_methods   → 0x03
// This test locks those bindings so a regression in either the C# generator
// or the C++ parser would fail loud.

#include <gtest/gtest.h>

#include "entitydef/entity_def_registry.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

// Encode a minimal descriptor matching TypeRegistryEmitter.EmitRegistrar.
// type_id is 16-bit, rpc list is shaped {name, rpc_id, param_count,
// [param_types], exposed}.
struct RpcSpec {
  std::string name;
  uint32_t rpc_id;
  ExposedScope exposed;
  std::vector<PropertyDataType> params;
};

auto MakeDescriptor(std::string_view type_name, uint16_t type_id, bool has_cell, bool has_client,
                    std::span<const RpcSpec> rpcs) -> std::vector<std::byte> {
  BinaryWriter w;
  w.WriteString(type_name);
  w.Write<uint16_t>(type_id);
  w.Write<uint8_t>(has_cell ? 1 : 0);
  w.Write<uint8_t>(has_client ? 1 : 0);
  w.WritePackedInt(0);  // 0 properties (Phase 10 RPC tests don't need them)
  w.WritePackedInt(static_cast<uint32_t>(rpcs.size()));
  for (const auto& r : rpcs) {
    w.WriteString(r.name);
    w.WritePackedInt(r.rpc_id);
    w.WritePackedInt(static_cast<uint32_t>(r.params.size()));
    for (auto pt : r.params) w.Write<uint8_t>(static_cast<uint8_t>(pt));
    w.Write<uint8_t>(static_cast<uint8_t>(r.exposed));
  }
  // Compression tail (internal/external): none.
  w.Write<uint8_t>(0);
  w.Write<uint8_t>(0);
  return w.Detach();
}

// Pack direction/typeIndex/methodIndex into an rpc_id the same way
// TypeRegistryEmitter does: (direction << 22) | (typeIndex << 8) | methodIndex.
constexpr uint32_t PackRpcId(uint8_t direction, uint16_t type_index, uint8_t method_index) {
  return (static_cast<uint32_t>(direction) << 22) | (static_cast<uint32_t>(type_index) << 8) |
         static_cast<uint32_t>(method_index);
}

class RegistryRpcTest : public ::testing::Test {
 protected:
  void SetUp() override { EntityDefRegistry::Instance().clear(); }
  void TearDown() override { EntityDefRegistry::Instance().clear(); }
};

}  // namespace

TEST_F(RegistryRpcTest, CellMethodDirectionExtracted) {
  const uint16_t kTypeId = 100;
  const uint32_t kCellRpcId = PackRpcId(/*direction=*/0x02, kTypeId, /*method=*/1);

  RpcSpec rpc{"MoveTo", kCellRpcId, ExposedScope::kAllClients, {}};
  auto blob = MakeDescriptor("Avatar", kTypeId, true, true, std::span{&rpc, 1});

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));

  const auto* desc = EntityDefRegistry::Instance().FindRpc(kCellRpcId);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->name, "MoveTo");
  EXPECT_EQ(desc->Direction(), 0x02)
      << "cell_methods must carry direction bits 0x02 so BaseApp's cell RPC "
         "path check can reject misrouted Base RPCs.";
  EXPECT_EQ(desc->exposed, ExposedScope::kAllClients);
  EXPECT_TRUE(desc->IsExposed());
  EXPECT_EQ(EntityDefRegistry::Instance().GetExposedScope(kCellRpcId), ExposedScope::kAllClients);
  EXPECT_TRUE(EntityDefRegistry::Instance().IsExposed(kCellRpcId));
}

TEST_F(RegistryRpcTest, BaseMethodDirectionExtracted) {
  const uint16_t kTypeId = 101;
  const uint32_t kBaseRpcId = PackRpcId(/*direction=*/0x03, kTypeId, /*method=*/1);

  RpcSpec rpc{"Save", kBaseRpcId, ExposedScope::kOwnClient, {}};
  auto blob = MakeDescriptor("Account", kTypeId, false, true, std::span{&rpc, 1});

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));

  const auto* desc = EntityDefRegistry::Instance().FindRpc(kBaseRpcId);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->Direction(), 0x03)
      << "base_methods must carry direction bits 0x03 so BaseApp's base RPC "
         "path check can reject misrouted Cell RPCs.";
  EXPECT_EQ(desc->exposed, ExposedScope::kOwnClient);
}

TEST_F(RegistryRpcTest, ClientMethodDirectionExtracted) {
  const uint16_t kTypeId = 102;
  // client_methods normally are not exposed (client is the CALLEE, not caller),
  // but the direction bits must still be present so the registry can reject
  // attempts to invoke them via ClientBaseRpc/ClientCellRpc channels.
  const uint32_t kClientRpcId = PackRpcId(/*direction=*/0x00, kTypeId, /*method=*/1);

  RpcSpec rpc{"OnDamage", kClientRpcId, ExposedScope::kNone, {}};
  auto blob = MakeDescriptor("Avatar", kTypeId, true, true, std::span{&rpc, 1});

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));

  const auto* desc = EntityDefRegistry::Instance().FindRpc(kClientRpcId);
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->Direction(), 0x00);
  EXPECT_EQ(desc->exposed, ExposedScope::kNone);
  EXPECT_FALSE(desc->IsExposed());
  EXPECT_FALSE(EntityDefRegistry::Instance().IsExposed(kClientRpcId));
}

TEST_F(RegistryRpcTest, NonExposedCellMethodReturnsKNone) {
  const uint16_t kTypeId = 103;
  const uint32_t kCellRpcId = PackRpcId(0x02, kTypeId, 2);

  RpcSpec rpc{"InternalOnly", kCellRpcId, ExposedScope::kNone, {}};
  auto blob = MakeDescriptor("Monster", kTypeId, true, false, std::span{&rpc, 1});

  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));

  EXPECT_EQ(EntityDefRegistry::Instance().GetExposedScope(kCellRpcId), ExposedScope::kNone);
  EXPECT_FALSE(EntityDefRegistry::Instance().IsExposed(kCellRpcId))
      << "A non-exposed method must reject client calls at BaseApp's first layer.";
}

TEST_F(RegistryRpcTest, UnknownRpcIdReturnsNullptr) {
  EXPECT_EQ(EntityDefRegistry::Instance().FindRpc(0xDEADBEEF), nullptr);
  EXPECT_EQ(EntityDefRegistry::Instance().GetExposedScope(0xDEADBEEF), ExposedScope::kNone);
  EXPECT_FALSE(EntityDefRegistry::Instance().IsExposed(0xDEADBEEF));
}

// ValidateRpc couples rpc_id to the registered type — cross-entity RPC calls
// (where the client sends rpc_id belonging to entity A but targets entity B)
// must be rejectable via this helper.
TEST_F(RegistryRpcTest, ValidateRpcRejectsWrongType) {
  const uint16_t kTypeA = 200;
  const uint16_t kTypeB = 201;
  const uint32_t kRpcOnA = PackRpcId(0x02, kTypeA, 1);

  RpcSpec rpc_a{"AMethod", kRpcOnA, ExposedScope::kAllClients, {}};
  auto blob_a = MakeDescriptor("EntityA", kTypeA, true, true, std::span{&rpc_a, 1});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterType(blob_a.data(),
                                                         static_cast<int32_t>(blob_a.size())));

  auto blob_b = MakeDescriptor("EntityB", kTypeB, true, true, {});
  ASSERT_TRUE(EntityDefRegistry::Instance().RegisterType(blob_b.data(),
                                                         static_cast<int32_t>(blob_b.size())));

  EXPECT_TRUE(EntityDefRegistry::Instance().ValidateRpc(kTypeA, kRpcOnA));
  EXPECT_FALSE(EntityDefRegistry::Instance().ValidateRpc(kTypeB, kRpcOnA));
}

}  // namespace atlas
