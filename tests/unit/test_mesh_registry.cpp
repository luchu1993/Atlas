#include "server/mesh_registry.h"

#include <string>

#include <gtest/gtest.h>

#include "foundation/process_type.h"
#include "network/address.h"
#include "network/machined_types.h"

namespace atlas {
namespace {

auto Proc(ProcessType type, std::string name, uint16_t id) -> machined::ProcessInfo {
  machined::ProcessInfo p;
  p.process_type = type;
  p.name = std::move(name);
  p.internal_addr = Address("10.0.0.1", id);
  p.pid = id;
  return p;
}

auto Owner(uint16_t port) -> Address { return Address("10.0.0.1", port); }

TEST(MeshRegistry, UpdateAndFindByType) {
  MeshRegistry reg;
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "cell1", 100),
                             Proc(ProcessType::kBaseApp, "base1", 101)});
  EXPECT_EQ(reg.OwnerCount(), 1u);
  EXPECT_EQ(reg.ProcessCount(), 2u);
  auto cells = reg.FindByType(ProcessType::kCellApp);
  ASSERT_EQ(cells.size(), 1u);
  EXPECT_EQ(cells[0].name, "cell1");
}

TEST(MeshRegistry, MergesAcrossOwners) {
  MeshRegistry reg;
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "cellA", 100)});
  reg.UpdateOwner(Owner(2), {Proc(ProcessType::kCellApp, "cellB", 200)});
  EXPECT_EQ(reg.OwnerCount(), 2u);
  EXPECT_EQ(reg.FindByType(ProcessType::kCellApp).size(), 2u);
}

TEST(MeshRegistry, UpdateReplacesOwnersProcesses) {
  MeshRegistry reg;
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "old", 100)});
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "new1", 101),
                             Proc(ProcessType::kCellApp, "new2", 102)});
  EXPECT_EQ(reg.OwnerCount(), 1u);
  EXPECT_EQ(reg.ProcessCount(), 2u);
  for (const auto& c : reg.FindByType(ProcessType::kCellApp)) EXPECT_NE(c.name, "old");
}

TEST(MeshRegistry, DropOwnerRemovesItsProcessesAndIsIdempotent) {
  MeshRegistry reg;
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "a", 100)});
  reg.UpdateOwner(Owner(2), {Proc(ProcessType::kCellApp, "b", 200)});
  EXPECT_TRUE(reg.DropOwner(Owner(1)));
  EXPECT_FALSE(reg.HasOwner(Owner(1)));
  EXPECT_EQ(reg.OwnerCount(), 1u);
  EXPECT_EQ(reg.FindByType(ProcessType::kCellApp).size(), 1u);
  EXPECT_FALSE(reg.DropOwner(Owner(1)));
}

TEST(MeshRegistry, EmptyUpdateDropsOwner) {
  MeshRegistry reg;
  reg.UpdateOwner(Owner(1), {Proc(ProcessType::kCellApp, "a", 100)});
  reg.UpdateOwner(Owner(1), {});
  EXPECT_FALSE(reg.HasOwner(Owner(1)));
  EXPECT_EQ(reg.OwnerCount(), 0u);
}

}  // namespace
}  // namespace atlas
