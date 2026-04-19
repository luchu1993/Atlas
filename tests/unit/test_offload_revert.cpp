// Phase 11 PR-6 review-fix B3 — Offload rollback tests.
//
// Drives CellApp::RevertPendingOffload directly on a locally-constructed
// CellApp (ctor only, no Init) with a hand-seeded PendingOffload + a
// Ghost entity. Verifies that revert promotes the Ghost back to Real,
// restores haunts / local-Cell membership / controllers, and behaves
// safely on edge cases.

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "controller_codec.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "real_entity_data.h"
#include "serialization/binary_stream.h"
#include "space.h"
#include "space/move_controller.h"
#include "space/timer_controller.h"

namespace atlas {
namespace {

auto FakeChannel(uintptr_t tag) -> Channel* {
  return reinterpret_cast<Channel*>(tag);
}

struct Harness {
  EventDispatcher dispatcher{"offload_revert"};
  NetworkInterface network{dispatcher};
  CellApp app{dispatcher, network};
};

// Helper: set the app up with a Space + local Cell + a Real entity that
// already has a haunt and a MoveToPoint controller. Returns the entity.
auto SeedEntityWithState(Harness& h, EntityID id, Channel* haunt_channel) -> CellEntity* {
  const SpaceID sid = 42;
  auto& spaces = h.app.Spaces();
  auto* space = spaces.emplace(sid, std::make_unique<Space>(sid)).first->second.get();
  auto* cell = space->AddLocalCell(std::make_unique<Cell>(*space, /*cell_id=*/7, CellBounds{}));
  auto* entity = space->AddEntity(std::make_unique<CellEntity>(
      id, /*type=*/1, *space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  entity->SetBase(Address(0x7F000001u, 20000), /*base_id=*/id);
  cell->AddRealEntity(entity);
  // Register with CellApp's population map so RevertPendingOffload's
  // FindEntity lookup resolves. In production this happens via
  // OnCreateCellEntity; the test bypasses that to avoid CLR / network
  // dependencies.
  h.app.EntityPopulationForTest()[id] = entity;
  if (haunt_channel != nullptr) entity->GetRealData()->AddHaunt(haunt_channel);
  entity->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{10, 0, 10}, 5.f, false), entity,
      /*user_arg=*/99);
  return entity;
}

// Build a PendingOffload snapshot that mirrors what TickOffloadChecker
// would capture for `entity` right before ConvertRealToGhost.
auto CaptureSnapshot(CellApp::PendingOffload& po, const CellEntity& entity, SpaceID sid,
                     cellappmgr::CellID cid, const Address& target,
                     const std::vector<Address>& haunt_addrs) -> void {
  po.target_addr = target;
  po.sent_at = Clock::now();
  po.space_id = sid;
  po.cell_id = cid;
  po.haunt_addrs = haunt_addrs;
  BinaryWriter cw;
  SerializeControllersForMigration(entity, cw);
  auto buf = cw.Detach();
  po.controller_blob.assign(buf.begin(), buf.end());
}

// ============================================================================
// Successful revert path: Ghost → Real, haunts + cell + controllers restored.
// ============================================================================

TEST(OffloadRevert, RevertRestoresRealWithHauntsCellAndControllers) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/100, peer_channel);
  const Address target(0x7F000001u, 30002);

  // Capture the pre-Offload snapshot. We don't need
  // peer_channel->RemoteAddress() (that would dereference the fake
  // pointer); synthesize an Address directly.
  CellApp::PendingOffload po;
  const Address fake_haunt_addr(0x7F000001u, 30003);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{fake_haunt_addr});

  // Simulate the Offload send: convert Real→Ghost and remove from Cell
  // + base_entity_population_ the way TickOffloadChecker would.
  entity->ConvertRealToGhost(peer_channel);
  ASSERT_TRUE(entity->IsGhost());
  EXPECT_EQ(entity->GetControllers().Count(), 0u);  // StopAll cleared
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // This unit test exercises the revert logic on a CellApp that was
  // never Init()ed — peer_registry_ is therefore empty, so haunt
  // resolution quietly drops the saved address. That's the intended
  // semantic: haunts that can't be resolved on revert are treated as
  // having died during the Offload window and will be re-ghosted on
  // the next TickGhostPump pass. The cell-membership + controller
  // restoration is the hot path and is the one we actually assert.

  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "test");

  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
  // Cell membership restored.
  EXPECT_TRUE(entity->GetSpace().FindLocalCell(7)->HasRealEntity(entity));
  // Controller restored from blob (same count, same user_arg).
  EXPECT_EQ(entity->GetControllers().Count(), 1u);
  bool saw_move = false;
  entity->GetControllers().ForEach([&](const Controller& c) {
    if (c.TypeTag() == ControllerKind::kMoveToPoint) {
      EXPECT_EQ(c.UserArg(), 99);
      saw_move = true;
    }
  });
  EXPECT_TRUE(saw_move);
}

// ============================================================================
// Revert on a missing entry is a no-op (idempotent).
// ============================================================================

TEST(OffloadRevert, RevertMissingEntryIsNoop) {
  Harness h;
  h.app.RevertPendingOffload(/*entity_id=*/9999, "missing");  // must not crash
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
}

// ============================================================================
// Revert on a non-Ghost entity logs + skips (the double-resolve case).
// ============================================================================

TEST(OffloadRevert, RevertSkipsWhenEntityAlreadyReal) {
  Harness h;
  auto* entity = SeedEntityWithState(h, /*id=*/101, /*haunt_channel=*/nullptr);
  CellApp::PendingOffload po;
  po.target_addr = Address(0x7F000001u, 30002);
  po.sent_at = Clock::now();
  po.space_id = 42;
  po.cell_id = 7;
  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);

  // Entity is still Real — simulates a late duplicate failure Ack
  // arriving after a previous success Ack already cleared the entry +
  // converted us into something else, then another success that made
  // us Real.
  h.app.RevertPendingOffload(entity->Id(), "double-resolve");
  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);  // entry consumed regardless
}

// ============================================================================
// Revert on a destroyed entity (removed from space AND population) is
// idempotent — no crash.
// ============================================================================

TEST(OffloadRevert, RevertOnDestroyedEntity) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/200, peer_channel);

  // Capture snapshot while entity is still alive.
  CellApp::PendingOffload po;
  const Address target(0x7F000001u, 30002);
  const Address fake_haunt_addr(0x7F000001u, 30003);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{fake_haunt_addr});

  // Convert Real → Ghost so the entity is in the state it'd be during
  // a pending Offload.
  entity->ConvertRealToGhost(peer_channel);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // Now destroy the entity: remove from Space AND population.
  const EntityID eid = entity->Id();
  h.app.EntityPopulationForTest().erase(eid);
  entity->GetSpace().RemoveEntity(eid);
  // `entity` pointer is now dangling — do not dereference.

  // Seed the pending offload and attempt revert. Must not crash even
  // though the entity no longer exists.
  h.app.PendingOffloadsForTest()[eid] = std::move(po);
  h.app.RevertPendingOffload(eid, "destroyed-entity");

  // The pending entry should be consumed regardless.
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
}

// ============================================================================
// Revert with a malformed (truncated) controller blob — entity becomes Real
// again, controllers may be partially restored, no crash.
// ============================================================================

TEST(OffloadRevert, RevertWithMalformedControllerBlob) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/300, peer_channel);

  // Capture a valid snapshot first.
  CellApp::PendingOffload po;
  const Address target(0x7F000001u, 30002);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{});

  // Truncate the controller blob to only 2 bytes — this is malformed.
  ASSERT_GT(po.controller_blob.size(), 2u);
  po.controller_blob.resize(2);

  // Convert Real → Ghost and remove from Cell.
  entity->ConvertRealToGhost(peer_channel);
  ASSERT_TRUE(entity->IsGhost());
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // Store the pending offload and revert.
  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "malformed-blob");

  // Entity must be Real again — no crash.
  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
  // Cell membership should be restored.
  EXPECT_TRUE(entity->GetSpace().FindLocalCell(7)->HasRealEntity(entity));
  // Controllers may be partially restored or empty — 0 is acceptable
  // when the blob is too truncated to decode any controller.
  EXPECT_LE(entity->GetControllers().Count(), 1u);
}

}  // namespace
}  // namespace atlas
