#include <gtest/gtest.h>

#include "movement_sim/movement_sim.h"
#include "physics_jolt/jolt_init.h"
#include "physics_jolt/jolt_physics_query.h"

namespace atlas::physics {

TEST(JoltPhysicsQuery, InitializeIsIdempotent) {
  jolt::Initialize();
  EXPECT_TRUE(jolt::IsInitialized());
  jolt::Initialize();
  EXPECT_TRUE(jolt::IsInitialized());
  jolt::Shutdown();
  EXPECT_FALSE(jolt::IsInitialized());
  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, EmptySceneReturnsNoHit) {
  jolt::Initialize();
  JoltPhysicsQuery query;

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, RaycastHitsStaticBox) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // 2m cube centered at origin: top face at y=1, ray from y=10 down should
  // hit at y=1 with distance=9 and normal=+Y.
  StaticBox box;
  box.min = {-1.0f, -1.0f, -1.0f};
  box.max = {1.0f, 1.0f, 1.0f};
  box.layer = 0;
  query.AddBox(box);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);

  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.distance_m, 9.0f, 1e-3f);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-3f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-3f);
  EXPECT_NEAR(hit.normal.x, 0.0f, 1e-3f);
  EXPECT_NEAR(hit.normal.z, 0.0f, 1e-3f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, RaycastMissesWhenAimedAway) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-1.0f, -1.0f, -1.0f};
  box.max = {1.0f, 1.0f, 1.0f};
  query.AddBox(box);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, 1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, ClearRemovesPriorBodies) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-1.0f, -1.0f, -1.0f};
  box.max = {1.0f, 1.0f, 1.0f};
  query.AddBox(box);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_TRUE(query.Raycast(rq).hit);

  query.Clear();
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, GroundProbeFindsBoxTop) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-1.0f, -1.0f, -1.0f};
  box.max = {1.0f, 1.0f, 1.0f};
  query.AddBox(box);

  GroundProbeQuery gp;
  gp.origin = {0.0f, 10.0f, 0.0f};
  gp.max_distance_m = 100.0f;
  gp.radius_m = 0.35f;
  auto hit = query.GroundProbe(gp);

  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.distance_m, 9.0f, 1e-2f);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, CastCapsuleStopsAtWall) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // Wall slab at x in [2,3], extending y in [0,4]
  StaticBox wall;
  wall.min = {2.0f, 0.0f, -5.0f};
  wall.max = {3.0f, 4.0f, 5.0f};
  query.AddBox(wall);

  CapsuleCastQuery cc;
  cc.capsule.center = {0.0f, 0.0f, 0.0f};
  cc.capsule.radius_m = 0.35f;
  cc.capsule.half_height_m = 0.9f;
  cc.displacement = {5.0f, 0.0f, 0.0f};
  auto hit = query.CastCapsule(cc);

  ASSERT_TRUE(hit.hit);
  EXPECT_LT(hit.fraction, 0.5f);  // first contact well before mid-cast
  EXPECT_NEAR(hit.normal.x, -1.0f, 0.1f);  // wall normal points back toward origin

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, OverlapCapsuleDetectsBodyInsideBox) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-5.0f, -5.0f, -5.0f};
  box.max = {5.0f, 5.0f, 5.0f};
  query.AddBox(box);

  OverlapQuery oq;
  oq.capsule.center = {0.0f, 0.0f, 0.0f};
  oq.capsule.radius_m = 0.35f;
  oq.capsule.half_height_m = 0.9f;
  EXPECT_TRUE(query.OverlapCapsule(oq));

  oq.capsule.center = {100.0f, 100.0f, 100.0f};
  EXPECT_FALSE(query.OverlapCapsule(oq));

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, DepenetrateCapsuleProducesSeparationVector) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-1.0f, -1.0f, -1.0f};
  box.max = {1.0f, 1.0f, 1.0f};
  query.AddBox(box);

  // Bottom hemisphere of the capsule pokes into the top of the box.
  OverlapQuery oq;
  oq.capsule.center = {0.0f, 0.95f, 0.0f};
  oq.capsule.radius_m = 0.35f;
  oq.capsule.half_height_m = 0.9f;
  auto hit = query.DepenetrateCapsule(oq);

  ASSERT_TRUE(hit.hit);
  EXPECT_GT(hit.depth_m, 0.0f);
  // Separation vector pushes capsule along its normal by depth.
  EXPECT_NEAR(hit.offset.x, hit.normal.x * hit.depth_m, 1e-3f);
  EXPECT_NEAR(hit.offset.y, hit.normal.y * hit.depth_m, 1e-3f);
  EXPECT_NEAR(hit.offset.z, hit.normal.z * hit.depth_m, 1e-3f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, MeshRaycastHitsTriangleAtKnownDepth) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  const math::Vector3 verts[] = {
      {-5.0f, 0.0f, -5.0f},
      { 5.0f, 0.0f, -5.0f},
      { 5.0f, 0.0f,  5.0f},
      {-5.0f, 0.0f,  5.0f},
  };
  // CW winding when viewed from +Y → +Y face normal (right-hand rule).
  const uint32_t indices[] = {0, 2, 1, 0, 3, 2};
  query.AddMesh(verts, indices, 0);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);

  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.distance_m, 10.0f, 1e-2f);
  EXPECT_NEAR(hit.position.y, 0.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, CookedMeshShapeRoundTripMatchesAddMesh) {
  jolt::Initialize();
  const math::Vector3 verts[] = {
      {-5.0f, 0.0f, -5.0f},
      { 5.0f, 0.0f, -5.0f},
      { 5.0f, 0.0f,  5.0f},
      {-5.0f, 0.0f,  5.0f},
  };
  const uint32_t indices[] = {0, 2, 1, 0, 3, 2};

  auto cooked = JoltPhysicsQuery::CookMeshShape(verts, indices);
  ASSERT_TRUE(cooked.HasValue()) << cooked.Error().Message();
  ASSERT_FALSE(cooked->empty());

  JoltPhysicsQuery query;
  auto added = query.AddCookedMeshShape(*cooked, 0);
  ASSERT_TRUE(added.HasValue()) << added.Error().Message();

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.distance_m, 10.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, CookMeshShapeRejectsBadIndices) {
  const math::Vector3 verts[] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                  {0.0f, 0.0f, 1.0f}};
  const uint32_t bad[] = {0, 1, 99};
  auto cooked = JoltPhysicsQuery::CookMeshShape(verts, bad);
  ASSERT_FALSE(cooked.HasValue());
  EXPECT_EQ(cooked.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(JoltPhysicsQuery, AddCookedMeshShapeRejectsEmptyBlob) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  std::vector<std::byte> empty;
  auto added = query.AddCookedMeshShape(std::span<const std::byte>(empty), 0);
  ASSERT_FALSE(added.HasValue());
  EXPECT_EQ(added.Error().Code(), ErrorCode::kInvalidArgument);
  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, CurrentJoltStampEncodesVersion) {
  // High 24 bits carry feature bits; the low 32 hold MAJOR<<16 | MINOR<<8 | PATCH.
  const uint64_t stamp = JoltPhysicsQuery::CurrentJoltStamp();
  const uint32_t version_part = static_cast<uint32_t>(stamp & 0xFFFFFFu);
  EXPECT_EQ((version_part >> 16) & 0xFFu, 5u);
  EXPECT_EQ((version_part >> 8) & 0xFFu, 2u);
}

// End-to-end: movement_sim::Step drives the capsule through PhysicsCharacterQuery
// onto a box top, registers grounded.
TEST(JoltPhysicsQuery, MovementStepFallsOntoBoxAndGrounds) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticBox box;
  box.min = {-5.0f, -1.0f, -5.0f};
  box.max = {5.0f, 1.0f, 5.0f};
  query.AddBox(box);

  movement::PhysicsCharacterQuery character_query(query, 2.0f, LayerMask{}, 0.35f);
  movement::MovementConfig config;
  movement::MovementState state;
  state.position = {0.0f, 5.0f, 0.0f};
  state.direction = {0.0f, 0.0f, 1.0f};
  state.flags = 0;

  movement::InputFrame input;
  input.client_dt_ms = 33;

  for (uint32_t tick = 1; tick <= 240 && (state.flags & movement::kMovementFlagGrounded) == 0;
       ++tick) {
    input.seq = tick;
    input.input_tick = tick;
    auto result = movement::Step(state, input, config, character_query, tick);
    state = result.state;
  }

  EXPECT_TRUE((state.flags & movement::kMovementFlagGrounded) != 0)
      << "capsule never grounded; final y=" << state.position.y;
  EXPECT_NEAR(state.position.y, 1.0f, 0.05f)
      << "capsule should rest on box top (y=1); final y=" << state.position.y;

  jolt::Shutdown();
}

}  // namespace atlas::physics
