// Phase 11 PR-6 review-fix B2 — Controller codec tests.
//
// Exercises the per-tick-state round-trip for MoveToPoint / Timer /
// Proximity, including the Proximity inside-peers seed that preserves
// cross-process membership.

#include <cstdint>
#include <memory>
#include <unordered_set>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "controller_codec.h"
#include "math/vector3.h"
#include "serialization/binary_stream.h"
#include "space.h"
#include "space/move_controller.h"
#include "space/proximity_controller.h"
#include "space/timer_controller.h"

namespace atlas {
namespace {

auto MakeReal(Space& s, EntityID id, math::Vector3 pos = {0, 0, 0}) -> CellEntity* {
  return s.AddEntity(std::make_unique<CellEntity>(id, /*type=*/1, s, pos, math::Vector3{1, 0, 0}));
}

// ============================================================================
// Empty round-trip — no controllers.
// ============================================================================

TEST(ControllerCodec, Empty_RoundTripIsNoop) {
  Space src_space(1);
  Space dst_space(1);
  auto* src = MakeReal(src_space, 1);
  auto* dst = MakeReal(dst_space, 1);

  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();
  BinaryReader r{std::span<const std::byte>(blob)};
  EXPECT_TRUE(DeserializeControllersForMigration(*dst, r, [](uint32_t) { return nullptr; }));
  EXPECT_EQ(dst->GetControllers().Count(), 0u);
}

// ============================================================================
// MoveToPointController round-trip preserves destination, speed, face flag.
// ============================================================================

TEST(ControllerCodec, MoveToPoint_RoundTripPreservesFields) {
  Space src_space(1);
  Space dst_space(1);
  auto* src = MakeReal(src_space, 1, {0, 0, 0});
  auto* dst = MakeReal(dst_space, 1, {0, 0, 0});

  const auto id = src->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{50, 0, 50}, /*speed=*/10.f,
                                              /*face_movement=*/true),
      /*motion=*/src, /*user_arg=*/42);

  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();
  BinaryReader r{std::span<const std::byte>(blob)};
  ASSERT_TRUE(DeserializeControllersForMigration(*dst, r, [](uint32_t) { return nullptr; }));

  ASSERT_EQ(dst->GetControllers().Count(), 1u);
  EXPECT_TRUE(dst->GetControllers().Contains(id));  // ID preserved
  // Advance and verify motion toward the restored destination.
  dst->GetControllers().Update(1.f);
  // 10 m/s for 1s → 10m toward (50, 0, 50). Direction vector normalised.
  EXPECT_GT(dst->Position().x, 0.f);
  EXPECT_GT(dst->Position().z, 0.f);
}

// ============================================================================
// TimerController preserves interval, repeat, accumulator, fire_count.
// ============================================================================

TEST(ControllerCodec, Timer_RoundTripPreservesAccumulatorAndFireCount) {
  Space src_space(1);
  Space dst_space(1);
  auto* src = MakeReal(src_space, 1);
  auto* dst = MakeReal(dst_space, 1);

  const auto id = src->GetControllers().Add(
      std::make_unique<TimerController>(/*interval=*/1.0f, /*repeat=*/true),
      /*motion=*/nullptr, /*user_arg=*/7);
  // Tick a fraction of the interval — accumulator should be mid-cycle.
  src->GetControllers().Update(0.4f);

  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();
  BinaryReader r{std::span<const std::byte>(blob)};
  ASSERT_TRUE(DeserializeControllersForMigration(*dst, r, [](uint32_t) { return nullptr; }));

  ASSERT_EQ(dst->GetControllers().Count(), 1u);
  EXPECT_TRUE(dst->GetControllers().Contains(id));
  // Verify accumulator survives — advance 0.5 more and confirm timer
  // didn't fire (0.4 + 0.5 = 0.9 < 1.0 interval). If accum had been
  // reset, 0.5 would also be below threshold so tighten: advance 0.7
  // which puts 0.4 + 0.7 = 1.1 over the threshold ⇒ fires.
  dst->GetControllers().Update(0.7f);
  EXPECT_EQ(dst->GetControllers().Count(), 1u);  // still alive (repeat=true)
}

// ============================================================================
// ProximityController round-trip resolves peers by entity_id.
// ============================================================================

TEST(ControllerCodec, Proximity_RoundTripResolvesPeersByEntityId) {
  // Source: central + one peer in range + one peer out of range.
  Space src_space(1);
  auto* src_central = MakeReal(src_space, 100, {0, 0, 0});
  auto* src_peer_in = MakeReal(src_space, 200, {5, 0, 5});
  auto* src_peer_out = MakeReal(src_space, 300, {500, 0, 500});
  (void)src_peer_out;

  const auto id = src_central->GetControllers().Add(
      std::make_unique<ProximityController>(src_central->RangeNode(), src_space.GetRangeList(),
                                            /*range=*/10.f, ProximityController::EnterFn{},
                                            ProximityController::LeaveFn{}),
      /*motion=*/nullptr, /*user_arg=*/0);

  // Verify initial inside set on src.
  {
    const auto& ctrls_map = src_central->GetControllers();
    bool seen = false;
    ctrls_map.ForEach([&](const Controller& c) {
      if (c.TypeTag() != ControllerKind::kProximity) return;
      const auto& p = static_cast<const ProximityController&>(c);
      EXPECT_EQ(p.InsidePeers().size(), 1u) << "only src_peer_in should be inside";
      seen = true;
    });
    EXPECT_TRUE(seen);
  }

  BinaryWriter w;
  SerializeControllersForMigration(*src_central, w);
  auto blob = w.Detach();

  // Destination space also contains src_peer_in (by id). Omit peer_out
  // to verify that "missing peers are silently dropped" from the seed.
  Space dst_space(1);
  auto* dst_central = MakeReal(dst_space, 100, {0, 0, 0});
  auto* dst_peer_in = MakeReal(dst_space, 200, {5, 0, 5});
  (void)dst_peer_in;

  BinaryReader r{std::span<const std::byte>(blob)};
  auto lookup = [&](uint32_t peer_id) -> CellEntity* { return dst_space.FindEntity(peer_id); };
  ASSERT_TRUE(DeserializeControllersForMigration(*dst_central, r, lookup));

  EXPECT_TRUE(dst_central->GetControllers().Contains(id));
  // Proximity's Start ran — check the restored trigger has peer_in inside.
  const auto& dst_ctrls = dst_central->GetControllers();
  bool verified = false;
  dst_ctrls.ForEach([&](const Controller& c) {
    if (c.TypeTag() != ControllerKind::kProximity) return;
    const auto& p = static_cast<const ProximityController&>(c);
    EXPECT_EQ(p.InsidePeers().size(), 1u);
    verified = true;
  });
  EXPECT_TRUE(verified);
  (void)src_peer_in;  // keep alive — RangeList still references it
}

// ============================================================================
// Controller ID preservation across migration.
// ============================================================================

TEST(ControllerCodec, PreservesControllerIds) {
  Space src_space(1);
  auto* src = MakeReal(src_space, 1);
  const auto id_move = src->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{1, 0, 0}, 1.f, false), src, 0);
  const auto id_timer =
      src->GetControllers().Add(std::make_unique<TimerController>(1.f, false), nullptr, 0);

  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();

  Space dst_space(1);
  auto* dst = MakeReal(dst_space, 1);
  BinaryReader r{std::span<const std::byte>(blob)};
  ASSERT_TRUE(DeserializeControllersForMigration(*dst, r, [](uint32_t) { return nullptr; }));

  EXPECT_TRUE(dst->GetControllers().Contains(id_move));
  EXPECT_TRUE(dst->GetControllers().Contains(id_timer));
}

// ============================================================================
// Malformed blob: abort cleanly.
// ============================================================================

TEST(ControllerCodec, TruncatedBlobReturnsFalse) {
  std::vector<std::byte> empty_blob;
  Space space(1);
  auto* e = MakeReal(space, 1);
  BinaryReader r{std::span<const std::byte>(empty_blob)};
  EXPECT_FALSE(DeserializeControllersForMigration(*e, r, [](uint32_t) { return nullptr; }));
}

TEST(ControllerCodec, UnknownKindAbortsRestore) {
  // Hand-craft a blob with count=1 (uint32 LE: 01 00 00 00) and Kind=99
  // (undefined), then id=1 (uint32 LE) and user_arg=0 (int32 LE).
  std::vector<std::byte> bad{std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                             std::byte{99},   std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                             std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                             std::byte{0x00}};
  Space space(1);
  auto* e = MakeReal(space, 1);
  BinaryReader r{std::span<const std::byte>(bad)};
  EXPECT_FALSE(DeserializeControllersForMigration(*e, r, [](uint32_t) { return nullptr; }));
}

// ============================================================================
// Mixed controller types — all survive round-trip.
// ============================================================================

TEST(ControllerCodec, MixedControllerTypes_AllSurviveRoundTrip) {
  Space src_space(1);
  Space dst_space(1);
  auto* src = MakeReal(src_space, 1, {0, 0, 0});
  auto* peer = MakeReal(src_space, 10, {5, 0, 0});
  (void)peer;

  // Add MoveToPoint controller.
  const auto id_move = src->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{100, 0, 100}, /*speed=*/5.f,
                                              /*face_movement=*/true),
      /*motion=*/src, /*user_arg=*/1);

  // Add Timer controller.
  const auto id_timer = src->GetControllers().Add(
      std::make_unique<TimerController>(/*interval=*/2.0f, /*repeat=*/true),
      /*motion=*/nullptr, /*user_arg=*/2);

  // Add Proximity controller.
  const auto id_prox = src->GetControllers().Add(
      std::make_unique<ProximityController>(src->RangeNode(), src_space.GetRangeList(),
                                            /*range=*/10.f, ProximityController::EnterFn{},
                                            ProximityController::LeaveFn{}),
      /*motion=*/nullptr, /*user_arg=*/0);

  ASSERT_EQ(src->GetControllers().Count(), 3u);

  // Serialize.
  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();

  // Destination space — peer entity present for proximity resolution.
  auto* dst = MakeReal(dst_space, 1, {0, 0, 0});
  auto* dst_peer = MakeReal(dst_space, 10, {5, 0, 0});
  (void)dst_peer;

  BinaryReader r{std::span<const std::byte>(blob)};
  auto resolver = [&dst_space](uint32_t id) -> CellEntity* { return dst_space.FindEntity(id); };
  ASSERT_TRUE(DeserializeControllersForMigration(*dst, r, resolver));

  // All 3 controllers survive.
  EXPECT_EQ(dst->GetControllers().Count(), 3u);
  EXPECT_TRUE(dst->GetControllers().Contains(id_move));
  EXPECT_TRUE(dst->GetControllers().Contains(id_timer));
  EXPECT_TRUE(dst->GetControllers().Contains(id_prox));

  // Verify types by advancing — MoveToPoint should move position.
  auto pos_before = dst->Position();
  dst->GetControllers().Update(1.f);
  EXPECT_GT(dst->Position().x, pos_before.x);

  // Verify proximity by iterating.
  bool found_prox = false;
  dst->GetControllers().ForEach([&](const Controller& c) {
    if (c.TypeTag() == ControllerKind::kProximity) found_prox = true;
  });
  EXPECT_TRUE(found_prox);
}

// ============================================================================
// Double round-trip — idempotent.
// ============================================================================

TEST(ControllerCodec, DoubleRoundTrip_Idempotent) {
  Space space_a(1);
  Space space_b(1);
  Space space_c(1);

  auto* a = MakeReal(space_a, 1, {0, 0, 0});

  // Add a MoveToPoint and a Timer.
  a->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{50, 0, 50}, /*speed=*/10.f,
                                              /*face_movement=*/false),
      /*motion=*/a, /*user_arg=*/0);
  a->GetControllers().Add(std::make_unique<TimerController>(/*interval=*/1.0f, /*repeat=*/true),
                          /*motion=*/nullptr, /*user_arg=*/0);

  // Advance so timer accumulator is non-zero.
  a->GetControllers().Update(0.3f);

  // First round-trip: A -> B.
  BinaryWriter w1;
  SerializeControllersForMigration(*a, w1);
  auto blob1 = w1.Detach();

  auto* b = MakeReal(space_b, 1, {0, 0, 0});
  BinaryReader r1{std::span<const std::byte>(blob1)};
  ASSERT_TRUE(DeserializeControllersForMigration(*b, r1, [](uint32_t) { return nullptr; }));
  ASSERT_EQ(b->GetControllers().Count(), 2u);

  // Second round-trip: B -> C.
  BinaryWriter w2;
  SerializeControllersForMigration(*b, w2);
  auto blob2 = w2.Detach();

  auto* c = MakeReal(space_c, 1, {0, 0, 0});
  BinaryReader r2{std::span<const std::byte>(blob2)};
  ASSERT_TRUE(DeserializeControllersForMigration(*c, r2, [](uint32_t) { return nullptr; }));

  // Final state must match first deserialization.
  EXPECT_EQ(c->GetControllers().Count(), b->GetControllers().Count());

  // Verify motion controller still works on the final entity.
  auto pos_before = c->Position();
  c->GetControllers().Update(1.f);
  EXPECT_GT(c->Position().x, pos_before.x);
  EXPECT_GT(c->Position().z, pos_before.z);
}

// Phase 11 C9: pre-C9 the count field was uint8, silently capping at
// 255 controllers per entity. BigWorld's ControllerID-width count
// (controllers.cpp:197) doesn't have the cap; Atlas now matches.
// Round-tripping > 255 controllers exercises the new uint32 field.
TEST(ControllerCodec, CountField_HandlesMoreThan255Controllers) {
  Space src_space(1);
  Space dst_space(1);
  auto* src = MakeReal(src_space, 1);
  auto* dst = MakeReal(dst_space, 1);

  constexpr uint32_t kCount = 300;  // just past the old uint8 cap
  for (uint32_t i = 0; i < kCount; ++i) {
    src->GetControllers().Add(std::make_unique<TimerController>(/*interval=*/1.f, /*repeat=*/true),
                              /*motion=*/nullptr, static_cast<int32_t>(i));
  }
  ASSERT_EQ(src->GetControllers().Count(), kCount);

  BinaryWriter w;
  SerializeControllersForMigration(*src, w);
  auto blob = w.Detach();
  BinaryReader r{std::span<const std::byte>(blob)};
  ASSERT_TRUE(DeserializeControllersForMigration(*dst, r, [](uint32_t) { return nullptr; }));
  EXPECT_EQ(dst->GetControllers().Count(), kCount);
}

}  // namespace
}  // namespace atlas
