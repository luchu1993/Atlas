// Cross-space teleport (cellapp side) tests.
//
// Drives CellApp::RequestTeleport / OnResolveSpaceHostReply / OnOffloadEntity
// on a ctor-only CellApp (no Init) with a hand-seeded Space + Real entity.
// Covers anchor-missing rejection, the resolve round-trip, the offload kick
// (Real->Ghost + OffloadEntity(is_teleport)), and the unhosted-target guard.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "foundation/intrusive_ptr.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"
#include "space.h"

namespace atlas {
namespace {

class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& d, InterfaceTable& t, const Address& r) : Channel(d, t, r) {}
  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Process-lifetime recording channel; channel holders use IntrusivePtr, so a
// stack object would crash on add_ref/release (mirrors FakeChannel).
auto RecordingChan() -> RecordingChannel* {
  static EventDispatcher d{"teleport-rec"};
  static InterfaceTable t;
  static std::vector<IntrusivePtr<RecordingChannel>> kept;
  kept.push_back(make_intrusive<RecordingChannel>(d, t, Address{}));
  return kept.back().get();
}

struct Harness {
  EventDispatcher dispatcher{"teleport-test"};
  NetworkInterface network{dispatcher};
  CellApp app{dispatcher, network};
  RecordingChannel* mgr_ch{RecordingChan()};

  Harness() { app.SeedCellAppMgrSessionForTest(mgr_ch, /*app_id=*/7, /*pid=*/101); }
};

auto MakeAddr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

// Real entity in `sid` with a local Cell + base_addr so it is offloadable.
auto SeedReal(Harness& h, EntityID id, SpaceID sid) -> CellEntity* {
  auto& spaces = h.app.Spaces();
  auto* space = spaces.emplace(sid, std::make_unique<Space>(sid)).first->second.get();
  auto* cell = space->AddLocalCell(std::make_unique<Cell>(*space, /*cell_id=*/7, CellBounds{}));
  auto* entity = space->AddEntity(std::make_unique<CellEntity>(
      id, /*type=*/uint16_t{1}, *space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  entity->SetBaseAddr(MakeAddr(20000));
  cell->AddRealEntity(entity);
  h.app.EntityPopulationForTest()[id] = entity;
  return entity;
}

auto CountRequests(const RecordingChannel& ch) -> std::size_t {
  std::size_t n = 0;
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (id && *id == cellappmgr::ResolveSpaceHostRequest::Descriptor().id) ++n;
  }
  return n;
}

auto FirstOffload(const RecordingChannel& ch) -> cellapp::OffloadEntity {
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (!id || *id != cellapp::OffloadEntity::Descriptor().id) continue;
    const auto len = r.ReadPackedInt();
    if (!len) continue;
    const auto payload = r.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader mr(*payload);
    auto msg = cellapp::OffloadEntity::Deserialize(mr);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No OffloadEntity in sends";
  return {};
}

auto FirstAck(const RecordingChannel& ch) -> cellapp::OffloadEntityAck {
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (!id || *id != cellapp::OffloadEntityAck::Descriptor().id) continue;
    auto msg = cellapp::OffloadEntityAck::Deserialize(r);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No OffloadEntityAck in sends";
  return {};
}

TEST(Teleport, RequestRejectsUnknownEntity) {
  Harness h;
  EXPECT_FALSE(h.app.RequestTeleport(/*entity_id=*/999, /*target=*/2, {1, 0, 1}, {1, 0, 0}));
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, RequestFailsWithoutMgrChannel) {
  Harness h;
  h.app.SeedCellAppMgrSessionForTest(nullptr, 7, 101);  // drop the mgr channel
  SeedReal(h, /*id=*/100, /*sid=*/1);
  EXPECT_FALSE(h.app.RequestTeleport(100, 2, {1, 0, 1}, {1, 0, 0}));
}

TEST(Teleport, RequestSendsResolveAndTracksPending) {
  Harness h;
  SeedReal(h, /*id=*/100, /*sid=*/1);
  EXPECT_TRUE(h.app.RequestTeleport(100, /*target_space=*/2, {5, 0, 6}, {1, 0, 0}));
  EXPECT_EQ(CountRequests(*h.mgr_ch), 1u);
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 1u);
}

TEST(Teleport, ResolveNotFoundAbortsAndKeepsReal) {
  Harness h;
  auto* entity = SeedReal(h, /*id=*/100, /*sid=*/1);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {5, 0, 6}, {1, 0, 0}));
  const uint32_t rid = h.app.PendingTeleportsForTest().begin()->first;

  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = false;
  h.app.OnResolveSpaceHostReply(MakeAddr(40000), h.mgr_ch, reply);

  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, ResolveUnknownRequestIsNoop) {
  Harness h;
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = 12345;  // never tracked
  reply.found = true;
  reply.host_addr = MakeAddr(40000);
  h.app.OnResolveSpaceHostReply(MakeAddr(40000), h.mgr_ch, reply);  // must not crash
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, ResolveFoundOffloadsToTargetAsTeleport) {
  Harness h;
  auto* entity = SeedReal(h, /*id=*/100, /*sid=*/1);
  const Address target = MakeAddr(40000);
  auto* peer_ch = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(target, peer_ch);

  ASSERT_TRUE(h.app.RequestTeleport(100, /*target_space=*/2, {7, 0, 8}, {1, 0, 0}));
  const uint32_t rid = h.app.PendingTeleportsForTest().begin()->first;

  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = true;
  reply.host_addr = target;
  reply.cell_id = 3;
  reply.geometry_version = 99;  // ignored; teleport ships geometry_version=0
  h.app.OnResolveSpaceHostReply(target, h.mgr_ch, reply);

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
  EXPECT_EQ(h.app.PendingOffloadsForTest().count(100), 1u);

  auto off = FirstOffload(*peer_ch);
  EXPECT_EQ(off.entity_id, 100u);
  EXPECT_TRUE(off.is_teleport);
  EXPECT_EQ(off.space_id, 2u);
  EXPECT_EQ(off.geometry_version, 0u);
  EXPECT_FLOAT_EQ(off.position.x, 7.f);
  EXPECT_FLOAT_EQ(off.position.z, 8.f);
}

TEST(Teleport, OnOffloadTeleportToUnhostedSpaceRejects) {
  Harness h;
  const Address src = MakeAddr(50000);
  auto* src_ch = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(src, src_ch);  // trust the sender

  cellapp::OffloadEntity msg;
  msg.entity_id = 100;
  msg.type_id = 1;
  msg.space_id = 777;  // not hosted on this CellApp
  msg.is_teleport = true;
  msg.geometry_version = 0;
  msg.base_addr = MakeAddr(20000);
  h.app.OnOffloadEntity(src, src_ch, msg);

  auto ack = FirstAck(*src_ch);
  EXPECT_EQ(ack.entity_id, 100u);
  EXPECT_FALSE(ack.success);
  EXPECT_EQ(ack.reject_reason, cellapp::OffloadRejectReason::kTargetMissing);
}

}  // namespace
}  // namespace atlas
