#include <cstddef>
#include <cstring>
#include <format>
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

TEST(CollisionAsset, LoadsSphereAndCapsule) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "sphere", "center": [1, 2, 3], "radius": 0.5, "layer": 4},
      {"shape": "capsule", "center": [0, 1, 0], "radius": 0.4, "half_height": 1.2, "layer": 5}
    ]
  })");
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  ASSERT_EQ(asset->spheres.size(), 1u);
  ASSERT_EQ(asset->capsules.size(), 1u);
  EXPECT_FLOAT_EQ(asset->spheres[0].center.y, 2.0f);
  EXPECT_FLOAT_EQ(asset->spheres[0].radius_m, 0.5f);
  EXPECT_EQ(asset->spheres[0].layer, 4u);
  EXPECT_FLOAT_EQ(asset->capsules[0].radius_m, 0.4f);
  EXPECT_FLOAT_EQ(asset->capsules[0].half_height_m, 1.2f);
  EXPECT_EQ(asset->capsules[0].layer, 5u);
}

TEST(CollisionAsset, RejectsSphereWithNonPositiveRadius) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [{"shape": "sphere", "center": [0, 0, 0], "radius": 0, "layer": 0}]
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

// ACOL side-car holding a 4-point tetrahedron (non-coplanar → valid hull).
[[nodiscard]] auto MakeTetraConvexBuffer() -> std::vector<std::byte> {
  const float verts[12] = {
      0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f,
  };
  std::vector<std::byte> bytes(kCollisionMeshBufferHeaderBytes + sizeof(verts));
  std::memcpy(bytes.data(), kCollisionMeshBufferMagic.data(), 4);
  const uint32_t version = kCollisionMeshBufferVersion;
  std::memcpy(bytes.data() + 4, &version, sizeof(version));
  std::memcpy(bytes.data() + kCollisionMeshBufferHeaderBytes, verts, sizeof(verts));
  return bytes;
}

TEST(CollisionAsset, LoadsConvexFromSideCar) {
  const auto buffer = MakeTetraConvexBuffer();
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "convex", "layer": 6, "vertex_byte_offset": 8, "vertex_count": 4}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  ASSERT_EQ(asset->convexes.size(), 1u);
  EXPECT_EQ(asset->convexes[0].vertices.size(), 4u);
  EXPECT_EQ(asset->convexes[0].layer, 6u);
}

// ACOL side-car holding an N*N flat height grid (all samples = `height`).
[[nodiscard]] auto MakeFlatHeightFieldBuffer(uint32_t n, float height)
    -> std::vector<std::byte> {
  const std::size_t count = std::size_t{n} * n;
  std::vector<std::byte> bytes(kCollisionMeshBufferHeaderBytes + count * sizeof(float));
  std::memcpy(bytes.data(), kCollisionMeshBufferMagic.data(), 4);
  const uint32_t version = kCollisionMeshBufferVersion;
  std::memcpy(bytes.data() + 4, &version, sizeof(version));
  std::vector<float> samples(count, height);
  std::memcpy(bytes.data() + kCollisionMeshBufferHeaderBytes, samples.data(),
              count * sizeof(float));
  return bytes;
}

TEST(CollisionAsset, LoadsHeightFieldFromSideCar) {
  const auto buffer = MakeFlatHeightFieldBuffer(4, 2.0f);
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "heightfield", "layer": 3, "origin": [0, 0, 0], "scale": [10, 1, 10],
       "sample_count": 4, "sample_byte_offset": 8}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  ASSERT_EQ(asset->heightfields.size(), 1u);
  EXPECT_EQ(asset->heightfields[0].sample_count, 4u);
  EXPECT_EQ(asset->heightfields[0].samples.size(), 16u);
  EXPECT_EQ(asset->heightfields[0].layer, 3u);
  EXPECT_FLOAT_EQ(asset->heightfields[0].scale.x, 10.0f);
}

TEST(CollisionAsset, RejectsHeightFieldWithTooFewSamples) {
  const auto buffer = MakeFlatHeightFieldBuffer(4, 0.0f);
  auto bad = [&](int n) {
    return LoadCollisionAssetFromJson(
        std::format(R"({{"version": 2,
          "coordinate_system": "x_right_y_up_z_forward_meters", "source_hash": "u",
          "objects": [{{"shape": "heightfield", "layer": 0, "origin": [0,0,0],
            "scale": [1,1,1], "sample_count": {}, "sample_byte_offset": 8}}]}})", n),
        std::span<const std::byte>(buffer));
  };
  EXPECT_FALSE(bad(3).HasValue());  // < 4
  EXPECT_FALSE(bad(2).HasValue());  // < 4
}

TEST(CollisionAsset, AcceptsOddSampleCountHeightField) {
  const auto buffer = MakeFlatHeightFieldBuffer(5, 1.0f);  // odd grid; backend pads the slack
  auto asset = LoadCollisionAssetFromJson(
      R"({"version": 2,
        "coordinate_system": "x_right_y_up_z_forward_meters", "source_hash": "u",
        "objects": [{"shape": "heightfield", "layer": 0, "origin": [0,0,0],
          "scale": [1,1,1], "sample_count": 5, "sample_byte_offset": 8}]})",
      std::span<const std::byte>(buffer));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  ASSERT_EQ(asset->heightfields.size(), 1u);
  EXPECT_EQ(asset->heightfields[0].sample_count, 5u);
  EXPECT_EQ(asset->heightfields[0].samples.size(), 25u);
}

TEST(CollisionAsset, RejectsConvexWithTooFewPoints) {
  const auto buffer = MakeTetraConvexBuffer();
  constexpr const char* kJson = R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [
      {"shape": "convex", "layer": 0, "vertex_byte_offset": 8, "vertex_count": 3}
    ]
  })";
  auto asset = LoadCollisionAssetFromJson(kJson, std::span<const std::byte>(buffer));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, RejectsCapsuleWithHalfHeightBelowRadius) {
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [{"shape": "capsule", "center": [0, 0, 0], "radius": 1.0,
                 "half_height": 0.5, "layer": 0}]
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, RejectsDegenerateCapsuleEqualToSphere) {
  // half_height == radius would build no Jolt body; reject at parse instead.
  auto asset = LoadCollisionAssetFromJson(R"({
    "version": 2,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "unit",
    "objects": [{"shape": "capsule", "center": [0, 0, 0], "radius": 1.0,
                 "half_height": 1.0, "layer": 0}]
  })");
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

TEST(CollisionAsset, CacheRoundTripBoxesPlanes) {
  const std::string source_hash = "unit";
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, source_hash);
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();
  EXPECT_EQ(asset->source_hash, source_hash);
  EXPECT_EQ(asset->boxes.size(), 1u);
  EXPECT_EQ(asset->planes.size(), 1u);
  EXPECT_TRUE(asset->meshes.empty());
}

TEST(CollisionAsset, CacheRoundTripWithMesh) {
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

TEST(CollisionAsset, CacheRoundTripCarriesStampAndCookedBlob) {
  const uint64_t stamp = 0x05020001ULL << 24 | 0x000123ULL;
  const std::vector<std::byte> cooked = {std::byte{0xDE}, std::byte{0xAD},
                                          std::byte{0xBE}, std::byte{0xEF}};
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "unit", stamp,
                                         std::span<const std::byte>(cooked));
  auto loaded = LoadCollisionCacheFromBytes(std::span<const std::byte>(bytes));
  ASSERT_TRUE(loaded.HasValue()) << loaded.Error().Message();
  EXPECT_EQ(loaded->jolt_version_stamp, stamp);
  ASSERT_EQ(loaded->cooked.size(), cooked.size());
  EXPECT_EQ(std::memcmp(loaded->cooked.data(), cooked.data(), cooked.size()), 0);
  EXPECT_EQ(loaded->asset.boxes.size(), 1u);
}

TEST(CollisionAsset, CacheLoadsLegacyV1Layout) {
  // Hand-rolled v1 layout: 16-byte header (magic + ver=1 + uint32 stamp + hashlen),
  // then hash + json_len + json + bin_len + bin. No cooked section.
  const std::string source_hash = "unit";
  const std::string json(kValidAsset);
  std::vector<std::byte> bytes;
  const std::array<char, 4> magic{'A', 'C', 'A', 'C'};
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(magic.data()),
               reinterpret_cast<const std::byte*>(magic.data()) + 4);
  auto append_u32 = [&](uint32_t v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    bytes.insert(bytes.end(), p, p + sizeof(v));
  };
  append_u32(1u);  // cache_version
  append_u32(0u);  // jolt_version_stamp (v1: uint32)
  append_u32(static_cast<uint32_t>(source_hash.size()));
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(source_hash.data()),
               reinterpret_cast<const std::byte*>(source_hash.data()) + source_hash.size());
  append_u32(static_cast<uint32_t>(json.size()));
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(json.data()),
               reinterpret_cast<const std::byte*>(json.data()) + json.size());
  append_u32(0u);  // bin_len = 0

  auto loaded = LoadCollisionCacheFromBytes(std::span<const std::byte>(bytes));
  ASSERT_TRUE(loaded.HasValue()) << loaded.Error().Message();
  EXPECT_EQ(loaded->jolt_version_stamp, 0u);
  EXPECT_TRUE(loaded->cooked.empty());
  EXPECT_EQ(loaded->asset.source_hash, source_hash);
  EXPECT_EQ(loaded->asset.boxes.size(), 1u);
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
  // Header stamps "X" but the embedded JSON's source_hash is "unit" — the
  // mismatch shape that indicates a corrupted cache header.
  auto bytes = WriteCollisionCacheBytes(kValidAsset, {}, "X");
  auto asset = LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(bytes));
  ASSERT_FALSE(asset.HasValue());
  EXPECT_EQ(asset.Error().Code(), ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace atlas::physics
