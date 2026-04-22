// Phase 11 PR-5 — CellAppMgr logic tests.
//
// These exercise the manager's bookkeeping paths (register, load, space
// creation, death, rebalance) without spinning up an EventDispatcher or
// real Channels. Handlers accept nullptr channels and the outbound-
// send helpers null-guard the channel field on CellAppInfo, so the
// state-side assertions are all we need. Protocol-level integration
// lives in PR-6.

#include <gtest/gtest.h>

#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/address.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"

namespace atlas {
namespace {

// Thin harness: real EventDispatcher + NetworkInterface so the inherited
// ServerApp ctors don't need refactoring. We never actually run the
// dispatcher — the test only pokes handlers directly.
struct CellAppMgrHarness {
  EventDispatcher dispatcher{"cellappmgr-test"};
  NetworkInterface network{dispatcher};
  CellAppMgr mgr{dispatcher, network};
};

auto MakePeerAddr(uint16_t port) -> Address {
  return Address(0x7F000001u, port);
}

// ============================================================================
// Register / Ack
// ============================================================================

TEST(CellAppMgr, Register_AssignsAppIdAndStoresPeer) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp msg;
  msg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(msg.internal_addr, /*ch=*/nullptr, msg);

  ASSERT_EQ(h.mgr.CellApps().size(), 1u);
  auto it = h.mgr.CellApps().find(msg.internal_addr);
  ASSERT_NE(it, h.mgr.CellApps().end());
  EXPECT_EQ(it->second.app_id, 1u);  // first CellApp → id 1 (0 reserved)
  EXPECT_EQ(it->second.internal_addr.Port(), 30001u);
}

TEST(CellAppMgr, Register_DuplicateIsRejected) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp msg;
  msg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  // Duplicate ignored — still one entry, still app_id 1.
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);
}

TEST(CellAppMgr, Register_MonotonicAppIds) {
  CellAppMgrHarness h;
  for (uint16_t i = 0; i < 5; ++i) {
    cellappmgr::RegisterCellApp msg;
    msg.internal_addr = MakePeerAddr(30001u + i);
    h.mgr.OnRegisterCellApp(msg.internal_addr, nullptr, msg);
  }
  ASSERT_EQ(h.mgr.CellApps().size(), 5u);
  // Collect app_ids and verify distinct, contiguous, starting at 1.
  std::vector<uint32_t> ids;
  for (const auto& [_, info] : h.mgr.CellApps()) ids.push_back(info.app_id);
  std::sort(ids.begin(), ids.end());
  for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], i + 1);
}

// ============================================================================
// InformCellLoad
// ============================================================================

TEST(CellAppMgr, InformCellLoad_UpdatesPeerAndLeafLoad) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  // Create a Space hosted on that peer so a BSP leaf exists.
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(42), 1u);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = 0.73f;
  load.entity_count = 42;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);

  const auto& info = h.mgr.CellApps().at(reg.internal_addr);
  EXPECT_NEAR(info.load, 0.73f, 1e-5f);
  EXPECT_EQ(info.entity_count, 42u);

  // Leaf picked up the load too.
  const auto& partition = h.mgr.Spaces().at(42);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_NEAR(leaves[0]->load, 0.73f, 1e-5f);
}

TEST(CellAppMgr, InformCellLoad_ClampsNegativeAndOverflow) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::InformCellLoad load;
  load.app_id = 1;
  load.load = -0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  EXPECT_FLOAT_EQ(h.mgr.CellApps().at(reg.internal_addr).load, 0.f);

  load.load = 5.0f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);
  EXPECT_FLOAT_EQ(h.mgr.CellApps().at(reg.internal_addr).load, 1.f);
}

TEST(CellAppMgr, InformCellLoad_UnknownAppIdIsIgnored) {
  CellAppMgrHarness h;
  cellappmgr::InformCellLoad load;
  load.app_id = 99;
  load.load = 0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load);  // must not crash
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

// ============================================================================
// CreateSpaceRequest — host selection + BSP seeding
// ============================================================================

TEST(CellAppMgr, CreateSpace_NoCellAppsAvailable_Drops) {
  CellAppMgrHarness h;
  cellappmgr::CreateSpaceRequest msg;
  msg.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, msg);
  EXPECT_TRUE(h.mgr.Spaces().empty());
}

TEST(CellAppMgr, CreateSpace_PicksLeastLoadedCellApp) {
  CellAppMgrHarness h;
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}, uint16_t{30003}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }
  // Make peer 30002 (app_id 2) lightest.
  cellappmgr::InformCellLoad l1;
  l1.app_id = 1;
  l1.load = 0.8f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l1);
  cellappmgr::InformCellLoad l2;
  l2.app_id = 2;
  l2.load = 0.1f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l2);
  cellappmgr::InformCellLoad l3;
  l3.app_id = 3;
  l3.load = 0.5f;
  h.mgr.OnInformCellLoad(Address{}, nullptr, l3);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 77;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(77), 1u);

  const auto& partition = h.mgr.Spaces().at(77);
  auto leaves = partition.bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, MakePeerAddr(30002));
}

TEST(CellAppMgr, CreateSpace_TieBrokenByLowestAppId) {
  CellAppMgrHarness h;
  // Two peers registered, both idle.
  for (uint16_t port : {uint16_t{30001}, uint16_t{30002}}) {
    cellappmgr::RegisterCellApp reg;
    reg.internal_addr = MakePeerAddr(port);
    h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  }
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  auto leaves = h.mgr.Spaces().at(1).bsp.Leaves();
  ASSERT_EQ(leaves.size(), 1u);
  EXPECT_EQ(leaves[0]->cellapp_addr, MakePeerAddr(30001));  // app_id 1 wins.
}

TEST(CellAppMgr, CreateSpace_DuplicateIsDropped) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  EXPECT_EQ(h.mgr.Spaces().size(), 1u);
}

// ============================================================================
// CellApp death
// ============================================================================

TEST(CellAppMgr, CellAppDeath_RemovesPeer) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);

  h.mgr.OnCellAppDeath(reg.internal_addr);
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

TEST(CellAppMgr, CellAppDeath_UnknownAddrSilent) {
  CellAppMgrHarness h;
  h.mgr.OnCellAppDeath(MakePeerAddr(9999));  // must not crash
  EXPECT_TRUE(h.mgr.CellApps().empty());
}

// Phase 11 C6: a death with surviving peers rehomes every orphaned leaf
// onto a survivor so BSP routing stays correct. BigWorld parity:
// CellApp::handleUnexpectedDeath (cellapp.cpp:312) picks findAlternateApp
// for each dead cell.
TEST(CellAppMgr, CellAppDeath_RehomesLeavesToSurvivor) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  // Space is created on whichever mgr picks — that's the to-be-killed
  // peer in this test (app_id 1 wins the tie on lowest app_id at zero
  // load). Assert the initial host then kill it.
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 7;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  ASSERT_EQ(h.mgr.Spaces().count(7), 1u);
  const auto& partition_before = h.mgr.Spaces().at(7);
  ASSERT_EQ(partition_before.bsp.Leaves().size(), 1u);
  const Address initial_host = partition_before.bsp.Leaves()[0]->cellapp_addr;
  ASSERT_EQ(initial_host, reg_a.internal_addr)
      << "PickHostForNewSpace should pick the lowest app_id under tied load";

  // Kill the initial host. The surviving peer (app_b) must end up as
  // the leaf's cellapp_addr.
  h.mgr.OnCellAppDeath(reg_a.internal_addr);

  const auto& partition_after = h.mgr.Spaces().at(7);
  ASSERT_EQ(partition_after.bsp.Leaves().size(), 1u);
  EXPECT_EQ(partition_after.bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr)
      << "dead leaf must rehome to the surviving peer";
  EXPECT_EQ(h.mgr.CellApps().size(), 1u);
  EXPECT_EQ(h.mgr.CellApps().begin()->second.internal_addr, reg_b.internal_addr);
}

// Phase 11 C6: multi-leaf Space — several leaves on the dead app all
// get rehomed to survivor(s) in a single pass. Verifies the loop
// doesn't bail after the first reassignment.
TEST(CellAppMgr, CellAppDeath_RehomesAllMatchingLeaves) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  // Two single-cell Spaces both hosted on app_a (tie break on app_id).
  cellappmgr::CreateSpaceRequest csr1;
  csr1.space_id = 10;
  csr1.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr1);
  cellappmgr::CreateSpaceRequest csr2;
  csr2.space_id = 11;
  csr2.request_id = 2;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr2);
  ASSERT_EQ(h.mgr.Spaces().at(10).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);
  ASSERT_EQ(h.mgr.Spaces().at(11).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);

  h.mgr.OnCellAppDeath(reg_a.internal_addr);

  EXPECT_EQ(h.mgr.Spaces().at(10).bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr);
  EXPECT_EQ(h.mgr.Spaces().at(11).bsp.Leaves()[0]->cellapp_addr, reg_b.internal_addr);
}

// Phase 11 C6: death with no survivors is log-only; leaves remain
// pointing at the dead addr so a subsequent CellApp join can optionally
// reclaim them (not implemented here). Without this guard the code
// would dereference a nullptr alt host.
TEST(CellAppMgr, CellAppDeath_LastPeerLeavesSpacesOrphaned) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 100;
  csr.request_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  h.mgr.OnCellAppDeath(reg_a.internal_addr);

  EXPECT_TRUE(h.mgr.CellApps().empty());
  // Space retained; leaf still claims the dead addr (defensive — a
  // reviving CellApp is the intended recovery path).
  ASSERT_EQ(h.mgr.Spaces().at(100).bsp.Leaves().size(), 1u);
  EXPECT_EQ(h.mgr.Spaces().at(100).bsp.Leaves()[0]->cellapp_addr, reg_a.internal_addr);
}

// ============================================================================
// TickLoadBalance — safety sanity
// ============================================================================

TEST(CellAppMgr, TickLoadBalance_EmptyIsSafe) {
  CellAppMgrHarness h;
  h.mgr.TickLoadBalance();  // no spaces, no crash
}

TEST(CellAppMgr, TickLoadBalance_SingleSpace_NoCrash) {
  // Single leaf (no internal nodes) → Balance is a no-op, but the
  // tick must still complete cleanly and broadcast the (unchanged)
  // geometry. We can't observe the broadcast here (channel is null);
  // the value is just crash-safety.
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);
  h.mgr.TickLoadBalance();
  EXPECT_EQ(h.mgr.Spaces().size(), 1u);
}

}  // namespace
}  // namespace atlas
