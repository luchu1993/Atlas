#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "math/vector3.h"
#include "movement_sim/movement_sim.h"
#include "physics/collision_asset.h"
#include "physics_jolt/jolt_collision_backend.h"
#include "physics_jolt/jolt_init.h"
#include "physics_jolt/jolt_physics_query.h"
#include "platform/filesystem.h"
#include "space.h"

// End-to-end proof for the cooked-cache runtime path: an atlas_tool-shaped
// .collisioncache on disk → Space::LoadCollisionCacheFromFile through the Jolt
// backend factory → server-authoritative movement that blocks against it.
namespace atlas {
namespace {

// view_yaw quantizes atan2(dir_x, dir_z); 65535/4 faces +X. move_z full forward.
constexpr uint16_t kFaceXPlusYaw = 16384;
constexpr int8_t kFullForward = 127;

auto WriteCacheFile(const std::string& name, std::span<const std::byte> bytes)
    -> std::filesystem::path {
  const auto path = fs::TempDirectory() / name;
  EXPECT_TRUE(fs::WriteFile(path, bytes).HasValue());
  return path;
}

auto MakeQuadAcolBuffer() -> std::vector<std::byte> {
  const float verts[12] = {-5.0f, 0.0f, -5.0f, 5.0f, 0.0f, -5.0f,
                           5.0f, 0.0f, 5.0f, -5.0f, 0.0f, 5.0f};
  const uint32_t indices[6] = {0, 2, 1, 0, 3, 2};
  std::vector<std::byte> bytes(physics::kCollisionMeshBufferHeaderBytes + sizeof(verts) +
                               sizeof(indices));
  std::memcpy(bytes.data(), physics::kCollisionMeshBufferMagic.data(), 4);
  const uint32_t version = physics::kCollisionMeshBufferVersion;
  std::memcpy(bytes.data() + 4, &version, sizeof(version));
  std::memcpy(bytes.data() + physics::kCollisionMeshBufferHeaderBytes, verts, sizeof(verts));
  std::memcpy(bytes.data() + physics::kCollisionMeshBufferHeaderBytes + sizeof(verts), indices,
              sizeof(indices));
  return bytes;
}

// Walks a grounded capsule +X for `ticks` and returns the final state.
auto WalkPlusX(const physics::PhysicsQuery& query, uint32_t ticks) -> movement::MovementState {
  movement::PhysicsCharacterQuery character_query(query, 2.0f, physics::LayerMask{}, 0.35f);
  movement::MovementConfig config;
  movement::MovementState state;
  state.position = {0.0f, 0.0f, 0.0f};
  state.direction = {1.0f, 0.0f, 0.0f};

  movement::InputFrame input;
  input.client_dt_ms = 33;
  input.view_yaw = kFaceXPlusYaw;
  input.move_z = kFullForward;
  for (uint32_t tick = 1; tick <= ticks; ++tick) {
    input.seq = tick;
    input.input_tick = tick;
    state = movement::Step(state, input, config, character_query, tick).state;
  }
  return state;
}

TEST(CollisionPipeline, CookedBoxCacheBlocksMovementAtWall) {
  physics::jolt::Initialize();
  // Ground covers the play area; a wall slab sits at x in [2, 3].
  constexpr const char* kJson = R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "wallmap",
    "objects": [
      {"shape": "box", "min": [-50, -1, -50], "max": [50, 0, 50], "layer": 0},
      {"shape": "box", "min": [2, 0, -50], "max": [3, 4, 50], "layer": 0}
    ]
  })";
  auto cooked = physics::JoltPhysicsQuery::CookCollisionMeshes({});
  ASSERT_TRUE(cooked.HasValue());
  auto bytes = physics::WriteCollisionCacheBytes(
      kJson, {}, "wallmap", physics::JoltPhysicsQuery::CurrentJoltStamp(),
      std::span<const std::byte>(*cooked));
  const auto path = WriteCacheFile("atlas_pipeline_wall.collisioncache",
                                   std::span<const std::byte>(bytes));

  Space space(1);
  space.SetCollisionBackendFactory(std::make_shared<physics::JoltCollisionBackendFactory>());
  auto loaded = space.LoadCollisionCacheFromFile(path);
  ASSERT_TRUE(loaded.HasValue()) << loaded.Error().Message();

  const auto state = WalkPlusX(space.PhysicsQuery(), 120);
  EXPECT_TRUE((state.flags & movement::kMovementFlagGrounded) != 0)
      << "capsule should stay grounded; final y=" << state.position.y;
  // Stopped at the wall face (x=2) minus capsule radius — never tunnelled through.
  EXPECT_GT(state.position.x, 1.2f) << "capsule did not travel toward the wall";
  EXPECT_LT(state.position.x, 2.0f) << "capsule tunnelled into/past the wall";

  std::filesystem::remove(path);
  physics::jolt::Shutdown();
}

TEST(CollisionPipeline, SpaceStaticCollisionAssetUsesChunkedQuery) {
  physics::CollisionAsset asset;
  asset.source_hash = "chunked_static";
  asset.boxes.push_back(
      physics::StaticBox{{10.2f, 0.0f, -2.0f}, {10.6f, 2.0f, 2.0f}, 1});

  Space space(1);
  space.SetCollisionAsset(asset);
  auto* chunked = dynamic_cast<physics::ChunkedPhysicsQuery*>(&space.PhysicsQuery());
  ASSERT_NE(chunked, nullptr);
  EXPECT_GT(chunked->ChunkCount(), 0u);

  physics::CapsuleCastQuery cast;
  cast.capsule.center = {9.0f, 0.0f, 0.0f};
  cast.capsule.radius_m = 0.35f;
  cast.capsule.half_height_m = 0.9f;
  cast.displacement = {2.0f, 0.0f, 0.0f};
  cast.filter.mask.bits = 1u << 1;

  EXPECT_TRUE(space.PhysicsQuery().CastCapsule(cast).hit);
}

TEST(CollisionPipeline, CookedGroundOnlyCacheAllowsTraversal) {
  physics::jolt::Initialize();
  // Control: same walk, no wall — the capsule must pass where the wall stood.
  constexpr const char* kJson = R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "groundmap",
    "objects": [
      {"shape": "box", "min": [-50, -1, -50], "max": [50, 0, 50], "layer": 0}
    ]
  })";
  auto cooked = physics::JoltPhysicsQuery::CookCollisionMeshes({});
  ASSERT_TRUE(cooked.HasValue());
  auto bytes = physics::WriteCollisionCacheBytes(
      kJson, {}, "groundmap", physics::JoltPhysicsQuery::CurrentJoltStamp(),
      std::span<const std::byte>(*cooked));
  const auto path = WriteCacheFile("atlas_pipeline_ground.collisioncache",
                                   std::span<const std::byte>(bytes));

  Space space(1);
  space.SetCollisionBackendFactory(std::make_shared<physics::JoltCollisionBackendFactory>());
  ASSERT_TRUE(space.LoadCollisionCacheFromFile(path).HasValue());

  const auto state = WalkPlusX(space.PhysicsQuery(), 120);
  EXPECT_TRUE((state.flags & movement::kMovementFlagGrounded) != 0);
  EXPECT_GT(state.position.x, 3.0f) << "capsule failed to traverse past x=3 with no wall";

  std::filesystem::remove(path);
  physics::jolt::Shutdown();
}

TEST(CollisionPipeline, CookedMeshCacheGroundsFromFile) {
  physics::jolt::Initialize();
  const auto bin = MakeQuadAcolBuffer();
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "meshmap",
    "objects": [
      {"shape": "mesh", "layer": 0,
       "vertex_byte_offset": 8, "vertex_count": 4,
       "index_byte_offset": 56, "index_count": 6}
    ]
  })";
  physics::MeshGeometry mesh;
  mesh.layer = 0;
  mesh.vertices = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f},
                   {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
  mesh.indices = {0, 2, 1, 0, 3, 2};
  auto cooked = physics::JoltPhysicsQuery::CookCollisionMeshes(
      std::span<const physics::MeshGeometry>(&mesh, 1));
  ASSERT_TRUE(cooked.HasValue());
  auto bytes = physics::WriteCollisionCacheBytes(
      kJson, std::span<const std::byte>(bin), "meshmap",
      physics::JoltPhysicsQuery::CurrentJoltStamp(), std::span<const std::byte>(*cooked));
  const auto path = WriteCacheFile("atlas_pipeline_mesh.collisioncache",
                                   std::span<const std::byte>(bytes));

  Space space(1);
  space.SetCollisionBackendFactory(std::make_shared<physics::JoltCollisionBackendFactory>());
  auto loaded = space.LoadCollisionCacheFromFile(path);
  ASSERT_TRUE(loaded.HasValue()) << loaded.Error().Message();

  // Capsule dropped from y=5 must land on the cooked-mesh ground at y=0.
  movement::PhysicsCharacterQuery character_query(space.PhysicsQuery(), 2.0f,
                                                  physics::LayerMask{}, 0.35f);
  movement::MovementConfig config;
  movement::MovementState state;
  state.position = {0.0f, 5.0f, 0.0f};
  state.direction = {0.0f, 0.0f, 1.0f};
  state.flags = 0;  // start airborne so the fall onto the mesh is exercised
  movement::InputFrame input;
  input.client_dt_ms = 33;
  for (uint32_t tick = 1;
       tick <= 240 && (state.flags & movement::kMovementFlagGrounded) == 0; ++tick) {
    input.seq = tick;
    input.input_tick = tick;
    state = movement::Step(state, input, config, character_query, tick).state;
  }
  EXPECT_TRUE((state.flags & movement::kMovementFlagGrounded) != 0)
      << "capsule never grounded on cooked mesh; final y=" << state.position.y;
  EXPECT_NEAR(state.position.y, 0.0f, 0.05f);

  std::filesystem::remove(path);
  physics::jolt::Shutdown();
}

}  // namespace
}  // namespace atlas
