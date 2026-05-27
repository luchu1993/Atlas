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

TEST(JoltPhysicsQuery, SkeletonReturnsNoHit) {
  ASSERT_TRUE(jolt::Initialize());
  JoltPhysicsQuery query;

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  GroundProbeQuery gp;
  gp.origin = {0.0f, 10.0f, 0.0f};
  gp.max_distance_m = 100.0f;
  gp.radius_m = 0.35f;
  EXPECT_FALSE(query.GroundProbe(gp).hit);

  CapsuleCastQuery cc;
  cc.capsule.center = {0.0f, 1.0f, 0.0f};
  cc.capsule.radius_m = 0.35f;
  cc.capsule.half_height_m = 0.9f;
  cc.displacement = {0.0f, -2.0f, 0.0f};
  EXPECT_FALSE(query.CastCapsule(cc).hit);

  OverlapQuery oq;
  oq.capsule = cc.capsule;
  EXPECT_FALSE(query.OverlapCapsule(oq));
  EXPECT_FALSE(query.DepenetrateCapsule(oq).hit);

  jolt::Shutdown();
}

}  // namespace atlas::physics
