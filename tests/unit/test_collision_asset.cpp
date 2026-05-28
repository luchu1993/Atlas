#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "physics/collision_asset.h"

namespace atlas::physics {
namespace {

// Builds a minimal v2 mesh buffer holding a single 2-triangle quad in the
// XZ plane at y=0 with vertex extent ±5.
[[nodiscard]] auto MakeQuadMeshBuffer() -> std::vector<std::byte> {
  const float verts[12] = {
      -5.0f, 0.0f, -5.0f,
       5.0f, 0.0f, -5.0f,
       5.0f, 0.0f,  5.0f,
      -5.0f, 0.0f,  5.0f,
  };
  // CW winding when viewed from +Y → +Y face normal (right-hand rule).
  const uint32_t indices[6] = {0, 2, 1, 0, 3, 2};
  std::vector<std::byte> bytes(kCollisionMeshBufferHeaderBytes + sizeof(verts) +
                               sizeof(indices));
  std::memcpy(bytes.data(), kCollisionMeshBufferMagic.data(), 4);
  const uint32_t version = kCollisionMeshBufferVersion;
  std::memcpy(bytes.data() + 4, &version, sizeof(version));
  std::memcpy(bytes.data() + kCollisionMeshBufferHeaderBytes, verts, sizeof(verts));
  std::memcpy(bytes.data() + kCollisionMeshBufferHeaderBytes + sizeof(verts), indices,
              sizeof(indices));
  return bytes;
}

constexpr const char* kValidAsset = R"({
  "version": 1,
  "coordinate_system": "x_right_y_up_z_forward_meters",
  "source_hash": "unit",
  "objects": [
    {"shape": "box", "min": [-1, 0, -1], "max": [1, 1, 1], "layer": 1},
    {"shape": "plane", "point": [0, -2, 0], "normal": [0, 1, 0], "layer": 2}
  ]
})";

TEST(CollisionAsset, LoadsBoxesAndPlanes) {
  auto asset = LoadCollisionAssetFromJson(kValidAsset);
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  EXPECT_EQ(asset->version, 1u);
  EXPECT_EQ(asset->source_hash, "unit");
  ASSERT_EQ(asset->boxes.size(), 1u);
  ASSERT_EQ(asset->planes.size(), 1u);
  EXPECT_FLOAT_EQ(asset->boxes[0].max.y, 1.0f);
  EXPECT_EQ(asset->planes[0].layer, 2u);
}

TEST(CollisionAsset, BuildsStaticPhysicsQuery) {
  auto asset = LoadCollisionAssetFromJson(kValidAsset);
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  auto query = BuildStaticPhysicsQueryFromAsset(*asset);
  GroundProbeQuery ground;
  ground.origin = {0.0f, 3.0f, 0.0f};
  ground.max_distance_m = 4.0f;
  ground.radius_m = 0.2f;
  ground.filter.mask.bits = 1u << 1;

  const auto hit = query->GroundProbe(ground);
  ASSERT_TRUE(hit.hit);
  EXPECT_FLOAT_EQ(hit.position.y, 1.0f);
  EXPECT_EQ(hit.layer, 1u);
}

TEST(CollisionAsset, DumpsObjPreview) {
  auto asset = LoadCollisionAssetFromJson(kValidAsset);
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  const auto obj = DumpCollisionAssetToObj(*asset);

  EXPECT_NE(obj.find("# source_hash unit"), std::string::npos);
  EXPECT_NE(obj.find("g box_0_layer_1"), std::string::npos);
  EXPECT_NE(obj.find("g plane_0_layer_2"), std::string::npos);
  EXPECT_NE(obj.find("v -1 0 -1"), std::string::npos);
  EXPECT_NE(obj.find("f 1 2 3 4"), std::string::npos);
  EXPECT_NE(obj.find("f 9 10 11 12"), std::string::npos);
}

TEST(CollisionAsset, BuildStaticPhysicsQueryDoesNotInjectFlatGround) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": []
  })");
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  auto query = BuildStaticPhysicsQueryFromAsset(*asset);
  GroundProbeQuery ground;
  ground.origin = {0.0f, 2.0f, 0.0f};
  ground.max_distance_m = 4.0f;
  ground.filter.mask.bits = 1u;

  EXPECT_FALSE(query->GroundProbe(ground).hit);
}

TEST(CollisionAsset, RejectsUnsupportedVersion) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 99,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": []
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kNotSupported);
}

TEST(CollisionAsset, RejectsInvalidBoxExtents) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "box", "min": [0, 0, 0], "max": [0, 1, 1], "layer": 1}
    ]
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, RejectsLayerOutsideMaskRange) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "plane", "point": [0, 0, 0], "normal": [0, 1, 0], "layer": 32}
    ]
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, RejectsMissingSourceHash) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "objects": []
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, V2LoadsMeshObjectFromSideCar) {
  const auto buffer = MakeQuadMeshBuffer();
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "mesh",
       "layer": 3,
       "vertex_byte_offset": 8,
       "vertex_count": 4,
       "index_byte_offset": 56,
       "index_count": 6}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  ASSERT_EQ(asset->meshes.size(), 1u);
  EXPECT_EQ(asset->meshes[0].layer, 3u);
  EXPECT_EQ(asset->meshes[0].vertices.size(), 4u);
  EXPECT_EQ(asset->meshes[0].indices.size(), 6u);
  EXPECT_FLOAT_EQ(asset->meshes[0].vertices[2].x, 5.0f);
  EXPECT_EQ(asset->meshes[0].indices[5], 2u);
}

TEST(CollisionAsset, V2MeshRequiresSideCarBuffer) {
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "mesh", "layer": 0,
       "vertex_byte_offset": 8, "vertex_count": 1,
       "index_byte_offset": 20, "index_count": 3}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson);
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, V2RejectsMeshBufferWithBadMagic) {
  auto buffer = MakeQuadMeshBuffer();
  buffer[0] = std::byte{'X'};
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": []
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, V2RejectsMeshIndexOutOfRange) {
  auto buffer = MakeQuadMeshBuffer();
  // Patch a single index to point past the vertex array.
  const uint32_t bad_index = 99;
  std::memcpy(buffer.data() + 56, &bad_index, sizeof(bad_index));
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "mesh", "layer": 0,
       "vertex_byte_offset": 8, "vertex_count": 4,
       "index_byte_offset": 56, "index_count": 6}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, V1AssetStillLoadsAfterVersionBump) {
  // Backward compat — version 1 JSON keeps working.
  auto asset = LoadCollisionAssetFromJson(kValidAsset);
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  EXPECT_EQ(asset->version, 1u);
}

TEST(CollisionAsset, CacheRoundTripV1) {
  const std::string source_hash = "unit";
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, source_hash);
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  EXPECT_EQ(asset->source_hash, source_hash);
  EXPECT_EQ(asset->boxes.size(), 1u);
  EXPECT_EQ(asset->planes.size(), 1u);
  EXPECT_TRUE(asset->meshes.empty());
}

TEST(CollisionAsset, CacheRoundTripV2WithMesh) {
  const auto mesh_buffer = MakeQuadMeshBuffer();
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "v2-cached",
    "objects": [
      {"shape": "mesh", "layer": 0,
       "vertex_byte_offset": 8, "vertex_count": 4,
       "index_byte_offset": 56, "index_count": 6}
    ]
  })";
  auto bytes = WriteCollisionCacheBytes(kJson, std::span<const std::byte>(mesh_buffer),
                                        "v2-cached");
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  EXPECT_EQ(asset->source_hash, "v2-cached");
  ASSERT_EQ(asset->meshes.size(), 1u);
  EXPECT_EQ(asset->meshes[0].vertices.size(), 4u);
  EXPECT_EQ(asset->meshes[0].indices.size(), 6u);
}

TEST(CollisionAsset, CacheRejectsBadMagic) {
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "unit");
  bytes[0] = std::byte{'X'};
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, CacheRejectsUnsupportedCacheVersion) {
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "unit");
  const uint32_t bad_version = 99;
  std::memcpy(bytes.data() + 4, &bad_version, sizeof(bad_version));
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kNotSupported);
}

TEST(CollisionAsset, CacheRejectsTruncatedHeader) {
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "unit");
  bytes.resize(8);  // chop off everything past magic + version
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_FALSE(asset.HasValue());
}

TEST(CollisionAsset, CacheRejectsCorruptSourceHashStamp) {
  // Cook with stamped hash 'X', but the embedded JSON says source_hash='unit'.
  // Loader must catch the mismatch since a corrupted cache stamp is the only way
  // these two get out of sync.
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "X");
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace atlas::physics
