// CellAppMgr logic tests.
//
// These exercise the manager's bookkeeping paths (register, load, space
// creation, death, rebalance) without spinning up an EventDispatcher or
// real Channels. Handlers accept nullptr channels and the outbound-send
// helpers null-guard the channel field on CellAppInfo, so the state-side
// assertions are all we need.

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "cellappmgr/bsp_tree.h"
#include "cellappmgr/cellappmgr.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "platform/io_poller.h"
#include "serialization/binary_stream.h"

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

// Minimal Channel subclass that captures the last frame written to
// DoSend. Lets tests observe outbound traffic without a live network.
class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote)
      : Channel(dispatcher, table, remote) {}

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }

  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Pull the first baseapp::CellAppDeath payload out of a RecordingChannel
// capture. Channel SendMessage frames carry a msg_id prefix + length
// header followed by the serialised struct; the InterfaceTable we ship
// to outbound sends uses the standard descriptor so the decoder here
// matches the test_watcher_forwarder helper shape.
auto FirstCellAppDeath(const RecordingChannel& ch) -> baseapp::CellAppDeath {
  EXPECT_FALSE(ch.Sends().empty());
  if (ch.Sends().empty()) return {};
  const auto& frame = ch.Sends().front();
  BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
  const auto id = reader.ReadPackedInt();
  EXPECT_TRUE(id.HasValue());
  EXPECT_EQ(*id, baseapp::CellAppDeath::Descriptor().id);
  const auto len = reader.ReadPackedInt();
  EXPECT_TRUE(len.HasValue());
  const auto payload = reader.ReadBytes(*len);
  EXPECT_TRUE(payload.HasValue());
  BinaryReader msg_reader(*payload);
  auto msg = baseapp::CellAppDeath::Deserialize(msg_reader);
  EXPECT_TRUE(msg.HasValue());
  return msg.ValueOr(baseapp::CellAppDeath{});
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

// A death with surviving peers rehomes every orphaned leaf onto a
// survivor so BSP routing stays correct.
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

// Multi-leaf Space: several leaves on the dead app all get rehomed to
// survivor(s) in a single pass. Verifies the loop doesn't bail after the
// first reassignment.
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

// Death with no survivors is log-only; leaves remain pointing at the
// dead addr so a subsequent CellApp join can optionally reclaim them
// (not implemented). Without this guard the code would deref a nullptr
// alt host.
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

// Asymmetric load across two BSP leaves must shift the split line
// toward the heavy side and then trigger a broadcast of the updated
// geometry. Walks the full integrated path:
//   Register two CellApps → CreateSpace → manually seed a two-cell BSP
//   (the mgr's production API doesn't yet expose a "split an existing
//   space" call; tests reach through SpacesForTest) → push asymmetric
//   InformCellLoad → call TickLoadBalance repeatedly → assert the BSP
//   split moved AND the broadcast-cache blob is different from the
//   pre-balance baseline.
TEST(CellAppMgr, TickLoadBalance_AsymmetricLoad_MovesSplitAndRebroadcasts) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // Seed a two-cell split: cell 1 stays on A, new cell 2 lands on B.
  // Production will expose a dedicated "split cell" admin RPC; the test
  // reaches past that gap because the rebalance logic it exercises is
  // independent of how the split got there.
  auto& partition = h.mgr.SpacesForTest().at(1);
  const uint32_t app_id_a = h.mgr.CellApps().at(reg_a.internal_addr).app_id;
  const uint32_t app_id_b = h.mgr.CellApps().at(reg_b.internal_addr).app_id;
  CellInfo new_cell{/*cell_id=*/2, reg_b.internal_addr, CellBounds{}, /*load=*/0.f,
                    /*entity_count=*/0};
  auto split = partition.bsp.Split(/*existing_cell_id=*/1, BSPAxis::kX, /*position=*/0.f, new_cell);
  ASSERT_TRUE(split.HasValue());

  // Baseline snapshot of the split line — this is what Balance should
  // shift when the load is heavier on the left side.
  const auto* leaf_left = partition.bsp.FindCell(-100.f, 0.f);
  const auto* leaf_right = partition.bsp.FindCell(+100.f, 0.f);
  ASSERT_NE(leaf_left, nullptr);
  ASSERT_NE(leaf_right, nullptr);
  EXPECT_EQ(leaf_left->cell_id, 1u);
  EXPECT_EQ(leaf_right->cell_id, 2u);

  // Baseline broadcast blob (CreateSpace's initial fan-out was with
  // the single-cell tree; our Split above didn't re-broadcast, so the
  // cache is out of date vs the current in-memory tree. That's fine —
  // the assertion below is "blob changes across the balance calls",
  // which it does either way).
  const auto baseline_blob = partition.last_broadcast_blob;

  // Feed asymmetric load: A (left half) heavy at 0.9, B (right half)
  // light at 0.1. Reported via wire message so the whole OnInformCellLoad
  // path runs, including cellapps_[] load update + per-leaf load mirror.
  cellappmgr::InformCellLoad load_a;
  load_a.app_id = app_id_a;
  load_a.load = 0.9f;
  load_a.entity_count = 900;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_a);
  cellappmgr::InformCellLoad load_b;
  load_b.app_id = app_id_b;
  load_b.load = 0.1f;
  load_b.entity_count = 100;
  h.mgr.OnInformCellLoad(Address{}, nullptr, load_b);

  // Several balance ticks: the damping aggression model moves a fraction
  // of the imbalance per pass, so we need multiple runs to see a clear
  // shift. Post-balance, the split should be strictly less than 0 (left
  // side shrank because left load was heavier).
  for (int i = 0; i < 5; ++i) h.mgr.TickLoadBalance();

  // Serialize fresh and compare. The cached blob must now differ from
  // the pre-balance state — or, equivalently, equal a freshly serialised
  // current tree.
  BinaryWriter w;
  partition.bsp.Serialize(w);
  const auto current_bytes_vec = w.Detach();
  const std::vector<std::byte> current_bytes(current_bytes_vec.begin(), current_bytes_vec.end());
  EXPECT_EQ(partition.last_broadcast_blob, current_bytes)
      << "cache should equal the freshly-serialised tree after rebalance";
  EXPECT_NE(partition.last_broadcast_blob, baseline_blob)
      << "rebalance should have changed the broadcast blob from its seeded baseline";

  // BSPInternal::Balance convention: when left is heavier, position_
  // increases (split moves right). Pre-balance origin (x=0) lives in
  // cell 2 (FindCell's `value < position` branches left; 0<0 is false
  // → right child → cell 2). Post-balance the split sits slightly right
  // of 0, so origin is now `< position` → cell 1. The QUALITATIVE
  // direction is locked here — the exact move size is aggression-
  // damped and would make a numeric assertion fragile.
  const auto* leaf_origin_after = partition.bsp.FindCell(0.f, 0.f);
  ASSERT_NE(leaf_origin_after, nullptr);
  EXPECT_EQ(leaf_origin_after->cell_id, 1u)
      << "with left heavier, split should have moved right past the origin";
}

// End-to-end CellApp-death recovery notification to subscribed BaseApps.
// Wire path: mgr's OnCellAppDeath rehomes the BSP leaves it was tracking,
// builds a baseapp::CellAppDeath with the per-Space new-host map, and
// fans it out to every subscribed BaseApp. Observing it needs a
// RecordingChannel injected via BaseAppChannelsForTest — the mgr's
// machined subscription normally fills that map from Birth events.
TEST(CellAppMgr, CellAppDeath_FansOutNotificationToBaseAppSubscribers) {
  CellAppMgrHarness h;
  // Two CellApps: A (will die) and B (will inherit A's leaves).
  cellappmgr::RegisterCellApp reg_a;
  reg_a.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg_a.internal_addr, nullptr, reg_a);
  cellappmgr::RegisterCellApp reg_b;
  reg_b.internal_addr = MakePeerAddr(30002);
  h.mgr.OnRegisterCellApp(reg_b.internal_addr, nullptr, reg_b);

  // A space hosted on A by default (OnCreateSpaceRequest picks the
  // least-loaded at tie-break, and app_id 1 wins ties).
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 42;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // Inject a recording baseapp peer so we can observe the fan-out.
  // Production wires this via the mgr's ProcessType::kBaseApp subscribe
  // callback; the test hook bypasses machined.
  InterfaceTable base_table;
  RecordingChannel base_ch(h.dispatcher, base_table, MakePeerAddr(20000));
  h.mgr.BaseAppChannelsForTest()[MakePeerAddr(20000)] = &base_ch;

  // Kill A. Mgr must (a) drop A from cellapps_, (b) rehome cell 1 to B,
  // (c) fan a CellAppDeath wire msg out to every baseapp — with the
  // rehomes list telling BaseApp which space moved where.
  h.mgr.OnCellAppDeath(reg_a.internal_addr);

  // BSP rehome (covered by other tests, but worth a sanity touch here
  // so the death-before-fan-out ordering is visible).
  const auto leaves = h.mgr.Spaces().at(42).bsp.Leaves();
  ASSERT_FALSE(leaves.empty());
  EXPECT_EQ(leaves[0]->cellapp_addr, reg_b.internal_addr);

  // Fan-out assertion: the baseapp peer received exactly one frame and
  // that frame decodes to a CellAppDeath naming A as dead and B as the
  // new host for space 42.
  ASSERT_EQ(base_ch.Sends().size(), 1u);
  const auto notify = FirstCellAppDeath(base_ch);
  EXPECT_EQ(notify.dead_addr, reg_a.internal_addr);
  ASSERT_EQ(notify.rehomes.size(), 1u);
  EXPECT_EQ(notify.rehomes[0].first, 42u);
  EXPECT_EQ(notify.rehomes[0].second, reg_b.internal_addr);
}

// The broadcast cache short-circuits re-sends when the serialised tree
// hasn't changed. Observable via SpacePartition::last_broadcast_blob:
// first fan-out populates it; subsequent no-op ticks must keep the
// bytes identical (we can't observe the wire directly with null
// channels, so byte-equality is the proxy). A BSP mutation forces a
// fresh cached blob.
TEST(CellAppMgr, BroadcastGeometry_CachesBlob_SkipsUnchangedReSends) {
  CellAppMgrHarness h;
  cellappmgr::RegisterCellApp reg;
  reg.internal_addr = MakePeerAddr(30001);
  h.mgr.OnRegisterCellApp(reg.internal_addr, nullptr, reg);
  cellappmgr::CreateSpaceRequest csr;
  csr.space_id = 1;
  h.mgr.OnCreateSpaceRequest(Address{}, nullptr, csr);

  // CreateSpace already fans out once; cache should now be populated.
  const auto& partition = h.mgr.Spaces().at(1);
  const auto baseline = partition.last_broadcast_blob;
  ASSERT_FALSE(baseline.empty());

  // Idle tick: blob must stay byte-identical. Any re-serialise would
  // produce a structurally equal but new vector — the equality still
  // holds because the cache tracks bytes, so this check also confirms
  // the tree itself didn't drift.
  h.mgr.TickLoadBalance();
  EXPECT_EQ(partition.last_broadcast_blob, baseline);
}

}  // namespace
}  // namespace atlas
