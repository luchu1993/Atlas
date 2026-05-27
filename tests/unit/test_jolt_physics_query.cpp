#include <gtest/gtest.h>

#include "physics_jolt/jolt_init.h"
#include "physics_jolt/jolt_physics_query.h"

namespace atlas::physics {

TEST(JoltPhysicsQuery, InitializeIsIdempotent) {
  ASSERT_TRUE(jolt::Initialize());
  EXPECT_TRUE(jolt::IsInitialized());
  ASSERT_TRUE(jolt::Initialize());
  EXPECT_TRUE(jolt::IsInitialized());
  jolt::Shutdown();
  EXPECT_FALSE(jolt::IsInitialized());
  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, EmptySceneReturnsNoHit) {
  ASSERT_TRUE(jolt::Initialize());
  JoltPhysicsQuery query;

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, RaycastHitsStaticBox) {
  ASSERT_TRUE(jolt::Initialize());
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
  ASSERT_TRUE(jolt::Initialize());
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
  ASSERT_TRUE(jolt::Initialize());
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

}  // namespace atlas::physics
