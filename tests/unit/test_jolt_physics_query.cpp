#include <limits>

#include <gtest/gtest.h>

#include "movement_sim/movement_sim.h"
#include "physics/collision_asset.h"
#include "physics_jolt/jolt_collision_backend.h"
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

TEST(JoltPhysicsQuery, CookCollisionMeshesEmptyInputYieldsHeaderOnly) {
  auto blob = JoltPhysicsQuery::CookCollisionMeshes({});
  ASSERT_TRUE(blob.HasValue()) << blob.Error().Message();
  ASSERT_EQ(blob->size(), sizeof(uint32_t));
}

TEST(JoltPhysicsQuery, CookCollisionMeshesRoundTripsTwoMeshes) {
  jolt::Initialize();

  std::vector<MeshGeometry> meshes(2);
  meshes[0].layer = 3;
  meshes[0].vertices = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f},
                         {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
  meshes[0].indices = {0, 2, 1, 0, 3, 2};
  meshes[1].layer = 7;
  meshes[1].vertices = {{-2.0f, 4.0f, -2.0f}, {2.0f, 4.0f, -2.0f},
                         {2.0f, 4.0f, 2.0f}, {-2.0f, 4.0f, 2.0f}};
  meshes[1].indices = {0, 2, 1, 0, 3, 2};

  auto blob = JoltPhysicsQuery::CookCollisionMeshes(meshes);
  ASSERT_TRUE(blob.HasValue()) << blob.Error().Message();

  JoltPhysicsQuery query;
  auto restored = query.RestoreCookedMeshes(*blob);
  ASSERT_TRUE(restored.HasValue()) << restored.Error().Message();
  EXPECT_EQ(*restored, 2u);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 4.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, RestoreCookedMeshesRejectsTruncatedBlob) {
  jolt::Initialize();
  std::vector<MeshGeometry> meshes(1);
  meshes[0].layer = 0;
  meshes[0].vertices = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f},
                         {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}};
  meshes[0].indices = {0, 2, 1, 0, 3, 2};
  auto blob = JoltPhysicsQuery::CookCollisionMeshes(meshes);
  ASSERT_TRUE(blob.HasValue());
  blob->resize(blob->size() / 2);

  JoltPhysicsQuery query;
  auto restored = query.RestoreCookedMeshes(*blob);
  ASSERT_FALSE(restored.HasValue());
  EXPECT_EQ(restored.Error().Code(), ErrorCode::kInvalidArgument);
  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, GroundProbeRespectsLayerMask) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // 2m cube on layer 1, top at y=1 (mirrors test_collision_asset Static test).
  query.AddBox(StaticBox{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1});

  GroundProbeQuery gp;
  gp.origin = {0.0f, 3.0f, 0.0f};
  gp.max_distance_m = 4.0f;
  gp.radius_m = 0.2f;
  gp.filter.mask.bits = 1u << 1;  // include layer 1
  auto hit = query.GroundProbe(gp);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);
  EXPECT_EQ(hit.layer, 1u);

  gp.filter.mask.bits = 1u;  // layer 0 only — excludes the layer-1 box
  EXPECT_FALSE(query.GroundProbe(gp).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, RaycastAndCastCapsuleRespectLayerMask) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  query.AddBox(StaticBox{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 2});

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  rq.filter.mask.bits = 1u << 1;  // excludes layer 2
  EXPECT_FALSE(query.Raycast(rq).hit);
  rq.filter.mask.bits = 1u << 2;  // includes layer 2
  auto rhit = query.Raycast(rq);
  ASSERT_TRUE(rhit.hit);
  EXPECT_EQ(rhit.layer, 2u);

  // A wall slab on layer 2; the capsule sweep into it is filtered the same way.
  JoltPhysicsQuery wall;
  wall.AddBox(StaticBox{{2.0f, 0.0f, -5.0f}, {3.0f, 4.0f, 5.0f}, 2});
  CapsuleCastQuery cc;
  cc.capsule.center = {0.0f, 0.0f, 0.0f};
  cc.capsule.radius_m = 0.35f;
  cc.capsule.half_height_m = 0.9f;
  cc.displacement = {5.0f, 0.0f, 0.0f};
  cc.filter.mask.bits = 1u << 1;  // excludes the layer-2 wall
  EXPECT_FALSE(wall.CastCapsule(cc).hit);
  cc.filter.mask.bits = 1u << 2;
  EXPECT_TRUE(wall.CastCapsule(cc).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddPlaneRaycastHitsGround) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  StaticPlane plane;
  plane.point = {0.0f, 0.0f, 0.0f};
  plane.normal = {0.0f, 1.0f, 0.0f};
  query.AddPlane(plane);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddSphereRaycastHitsTop) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // Unit sphere centered at origin: top at y=1, ray from y=10 down hits there.
  query.AddSphere(StaticSphere{{0.0f, 0.0f, 0.0f}, 1.0f, 0});

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddCapsuleOverlapDetectsInsideOutside) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // Vertical capsule, geometric center at origin, total half-height 1.0, r=0.5.
  query.AddCapsule(StaticCapsule{{0.0f, 0.0f, 0.0f}, 0.5f, 1.0f, 0});

  OverlapQuery oq;
  oq.capsule.center = {0.0f, -0.9f, 0.0f};  // foot near the obstacle center
  oq.capsule.radius_m = 0.35f;
  oq.capsule.half_height_m = 0.9f;
  EXPECT_TRUE(query.OverlapCapsule(oq));

  oq.capsule.center = {50.0f, 0.0f, 50.0f};
  EXPECT_FALSE(query.OverlapCapsule(oq));

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddConvexHullRaycastHitsBoxHull) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  // 8 corners of a 2m cube centered at origin → hull top at y=1.
  const math::Vector3 corners[8] = {
      {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
      {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
  query.AddConvexHull(corners, 0);

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddConvexHullRejectsDegeneratePointSet) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  const math::Vector3 too_few[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  query.AddConvexHull(too_few, 0);  // < 4 points → no body added

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, BuildsConvexCacheWithoutStampDependency) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "convex";
  ConvexGeometry hull;
  hull.layer = 0;
  hull.vertices = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                   {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
  cache.asset.convexes.push_back(hull);
  cache.jolt_version_stamp = 0;  // no meshes → stamp irrelevant

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = (*query)->Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, BuildsSphereAndCapsuleCacheWithoutStampDependency) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "shapes";
  cache.asset.spheres.push_back(StaticSphere{{0.0f, 0.0f, 0.0f}, 1.0f, 0});
  cache.asset.capsules.push_back(StaticCapsule{{5.0f, 0.0f, 0.0f}, 0.5f, 1.5f, 0});
  cache.jolt_version_stamp = 0;  // no meshes → stamp irrelevant

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = (*query)->Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 1.0f, 1e-2f);  // sphere top

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddHeightFieldRaycastHitsFlatTerrain) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  HeightFieldGeometry hf;
  hf.origin = {0.0f, 0.0f, 0.0f};
  hf.scale = {10.0f, 1.0f, 10.0f};  // 4x4 grid spans [0,30]x[0,30]
  hf.sample_count = 4;
  hf.samples.assign(16, 2.0f);  // flat terrain at y=2
  hf.layer = 0;
  query.AddHeightField(hf);

  RaycastQuery rq;
  rq.origin = {15.0f, 10.0f, 15.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = query.Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 2.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, AddHeightFieldHoleSamplesHaveNoCollision) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  HeightFieldGeometry hf;
  hf.origin = {0.0f, 0.0f, 0.0f};
  hf.scale = {10.0f, 1.0f, 10.0f};
  hf.sample_count = 4;
  hf.samples.assign(16, std::numeric_limits<float>::max());  // all holes
  hf.layer = 0;
  query.AddHeightField(hf);

  RaycastQuery rq;
  rq.origin = {15.0f, 10.0f, 15.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_FALSE(query.Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, HeightFieldRaycastTracksColumnHeightAndAcceptsOddCount) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  HeightFieldGeometry hf;
  hf.origin = {0.0f, 0.0f, 0.0f};
  hf.scale = {10.0f, 1.0f, 10.0f};  // 5x5 grid (odd) spans [0,40]x[0,40]
  hf.sample_count = 5;              // odd: Jolt rounds the sample count up internally
  hf.samples.resize(25);
  // Height rises with the x column only (flat along z): surface plane y = 2 + x_index.
  for (uint32_t z = 0; z < 5; ++z)
    for (uint32_t x = 0; x < 5; ++x) hf.samples[z * 5 + x] = 2.0f + static_cast<float>(x);
  hf.layer = 0;
  query.AddHeightField(hf);

  auto height_at = [&](float wx, float wz) {
    RaycastQuery rq;
    rq.origin = {wx, 100.0f, wz};
    rq.direction = {0.0f, -1.0f, 0.0f};
    rq.max_distance_m = 200.0f;
    return query.Raycast(rq);
  };
  // world x=15 → x_index 1.5 → y=3.5; world x=25 → x_index 2.5 → y=4.5. Tied to x, not z:
  // a z/x transpose would make both (same z=15) equal and fail.
  const auto a = height_at(15.0f, 15.0f);
  const auto b = height_at(25.0f, 15.0f);
  ASSERT_TRUE(a.hit);
  ASSERT_TRUE(b.hit);
  EXPECT_NEAR(a.position.y, 3.5f, 1e-2f);
  EXPECT_NEAR(b.position.y, 4.5f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltPhysicsQuery, GroundProbeStandsOnHeightFieldTerrain) {
  jolt::Initialize();
  JoltPhysicsQuery query;
  HeightFieldGeometry hf;
  hf.origin = {0.0f, 0.0f, 0.0f};
  hf.scale = {10.0f, 1.0f, 10.0f};
  hf.sample_count = 4;
  hf.samples.assign(16, 3.0f);  // flat terrain at y=3
  hf.layer = 0;
  query.AddHeightField(hf);

  GroundProbeQuery gp;
  gp.origin = {15.0f, 10.0f, 15.0f};
  gp.max_distance_m = 100.0f;
  gp.radius_m = 0.35f;
  const auto hit = query.GroundProbe(gp);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 3.0f, 1e-2f);
  EXPECT_NEAR(hit.normal.y, 1.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, BuildsHeightFieldCacheWithoutStampDependency) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "terrain";
  HeightFieldGeometry hf;
  hf.origin = {0.0f, 0.0f, 0.0f};
  hf.scale = {10.0f, 1.0f, 10.0f};
  hf.sample_count = 4;
  hf.samples.assign(16, 0.0f);
  hf.layer = 0;
  cache.asset.heightfields.push_back(hf);
  cache.jolt_version_stamp = 0;  // no meshes → stamp irrelevant

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  RaycastQuery rq;
  rq.origin = {15.0f, 10.0f, 15.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = (*query)->Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, RejectsCacheWhoseShapeFailsToBuild) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "bad";
  cache.asset.boxes.push_back(StaticBox{{-1, -1, -1}, {1, 1, 1}, 0});
  // 4 coincident points (zero extent) can't form a hull → no body built.
  ConvexGeometry degenerate;
  degenerate.layer = 0;
  degenerate.vertices = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  cache.asset.convexes.push_back(degenerate);

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_FALSE(query.HasValue());  // 1 of 2 shapes built → surfaced, not silent
  EXPECT_EQ(query.Error().Code(), ErrorCode::kInvalidArgument);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, BuildsBoxOnlyCacheWithoutStampDependency) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "box";
  cache.asset.boxes.push_back(StaticBox{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0});
  cache.jolt_version_stamp = 0;  // irrelevant when there are no cooked meshes

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  EXPECT_TRUE((*query)->Raycast(rq).hit);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, BuildsMeshCacheFromCookedBlob) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "mesh";
  MeshGeometry mesh;
  mesh.layer = 0;
  mesh.vertices = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f},
                   {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
  mesh.indices = {0, 2, 1, 0, 3, 2};
  cache.asset.meshes.push_back(mesh);
  auto cooked = JoltPhysicsQuery::CookCollisionMeshes(cache.asset.meshes);
  ASSERT_TRUE(cooked.HasValue());
  cache.cooked = *cooked;
  cache.jolt_version_stamp = JoltPhysicsQuery::CurrentJoltStamp();

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  RaycastQuery rq;
  rq.origin = {0.0f, 10.0f, 0.0f};
  rq.direction = {0.0f, -1.0f, 0.0f};
  rq.max_distance_m = 100.0f;
  auto hit = (*query)->Raycast(rq);
  ASSERT_TRUE(hit.hit);
  EXPECT_NEAR(hit.position.y, 0.0f, 1e-2f);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, RejectsMeshCacheWithStaleStamp) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "mesh";
  MeshGeometry mesh;
  mesh.vertices = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f},
                   {1.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 1.0f}};
  mesh.indices = {0, 2, 1, 0, 3, 2};
  cache.asset.meshes.push_back(mesh);
  cache.cooked = *JoltPhysicsQuery::CookCollisionMeshes(cache.asset.meshes);
  cache.jolt_version_stamp = JoltPhysicsQuery::CurrentJoltStamp() ^ 0xFFu;

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_FALSE(query.HasValue());
  EXPECT_EQ(query.Error().Code(), ErrorCode::kNotSupported);

  jolt::Shutdown();
}

TEST(JoltCollisionBackend, RejectsMeshCacheWithMissingCookedBlob) {
  jolt::Initialize();
  LoadedCollisionCache cache;
  cache.asset.source_hash = "mesh";
  MeshGeometry mesh;
  mesh.vertices = {{-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 1.0f}};
  mesh.indices = {0, 2, 1};
  cache.asset.meshes.push_back(mesh);
  cache.jolt_version_stamp = JoltPhysicsQuery::CurrentJoltStamp();
  // cooked deliberately left empty

  JoltCollisionBackendFactory factory;
  auto query = factory.BuildFromCache(cache);
  ASSERT_FALSE(query.HasValue());
  EXPECT_EQ(query.Error().Code(), ErrorCode::kInvalidArgument);

  jolt::Shutdown();
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
