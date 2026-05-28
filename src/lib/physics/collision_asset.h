#ifndef ATLAS_LIB_PHYSICS_COLLISION_ASSET_H_
#define ATLAS_LIB_PHYSICS_COLLISION_ASSET_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"
#include "math/vector3.h"
#include "physics/physics_query.h"

namespace atlas::physics {

inline constexpr uint32_t kCollisionAssetVersion = 2;
inline constexpr std::string_view kCollisionCoordinateSystem =
    "x_right_y_up_z_forward_meters";

// Side-car binary layout (little-endian throughout):
//   bytes 0..3   magic 'A','C','O','L'
//   bytes 4..7   uint32 version (currently kCollisionMeshBufferVersion)
//   bytes 8..    raw float32 vertices and uint32 indices, addressed by
//                per-object byte offsets in the JSON. Offsets are absolute
//                file offsets so the header itself does not need to be
//                skipped manually.
inline constexpr std::array<char, 4> kCollisionMeshBufferMagic{'A', 'C', 'O', 'L'};
inline constexpr uint32_t kCollisionMeshBufferVersion = 1;
inline constexpr std::size_t kCollisionMeshBufferHeaderBytes = 8;

// Cooked-cache format (M5). Bundles a stamped header + the source JSON + the
// mesh side-car bytes into a single self-contained file:
//   bytes 0..3    magic 'A','C','A','C'
//   bytes 4..7    uint32 kCollisionCacheVersion
//   bytes 8..11   uint32 jolt_version_stamp (reserved; 0 until M5b pre-cooks
//                 Jolt shapes, at which point this field invalidates caches
//                 across Jolt upgrades)
//   bytes 12..15  uint32 source_hash_len
//   then          source_hash bytes
//   then          uint32 json_len + json bytes
//   then          uint32 bin_len + bin bytes
inline constexpr std::array<char, 4> kCollisionCacheMagic{'A', 'C', 'A', 'C'};
inline constexpr uint32_t kCollisionCacheVersion = 1;
inline constexpr std::size_t kCollisionCacheHeaderBytes = 16;

struct MeshGeometry {
  std::vector<math::Vector3> vertices;
  std::vector<uint32_t> indices;  // triangle list; size % 3 == 0
  ObjectLayer layer{0};
};

struct CollisionAsset {
  uint32_t version{kCollisionAssetVersion};
  std::string coordinate_system{std::string(kCollisionCoordinateSystem)};
  std::string source_hash;
  std::vector<StaticBox> boxes;
  std::vector<StaticPlane> planes;
  std::vector<MeshGeometry> meshes;
};

// v1 assets (boxes / planes only) and v2 assets without mesh objects.
[[nodiscard]] auto LoadCollisionAssetFromJson(std::string_view json)
    -> Result<CollisionAsset>;

// v2 assets that reference mesh data; pass the side-car bytes as the second
// argument. Returns InvalidArgument if mesh objects are present but the
// buffer is empty / missing the magic / too short for the recorded offsets.
[[nodiscard]] auto LoadCollisionAssetFromJson(std::string_view json,
                                              std::span<const std::byte> mesh_buffer)
    -> Result<CollisionAsset>;

// Reads `path` plus, if needed, the side-car derived as `path` with the
// extension swapped from .json to .bin (e.g. foo.collision.json →
// foo.collision.bin). Missing side-car when no mesh objects exist is fine.
[[nodiscard]] auto LoadCollisionAssetFromFile(const std::filesystem::path& path)
    -> Result<CollisionAsset>;

[[nodiscard]] auto BuildStaticPhysicsQueryFromAsset(const CollisionAsset& asset)
    -> std::unique_ptr<StaticPhysicsQuery>;
[[nodiscard]] auto DumpCollisionAssetToObj(const CollisionAsset& asset) -> std::string;

// Cache cooking and loading. The cache is a self-contained bundle of the
// source JSON + side-car bytes with a stamped header. Runtime that loads
// from cache only needs the .collisioncache file; mismatched magic /
// cache_version / jolt_version_stamp causes a hard load failure (the spec
// requires "refuse on mismatch", no silent fallback).
[[nodiscard]] auto WriteCollisionCacheBytes(std::string_view source_json,
                                            std::span<const std::byte> source_bin,
                                            std::string_view source_hash)
    -> std::vector<std::byte>;
[[nodiscard]] auto WriteCollisionCacheToFile(const std::filesystem::path& source_json_path,
                                              const std::filesystem::path& cache_path)
    -> Result<void>;
[[nodiscard]] auto LoadCollisionAssetFromCacheBytes(std::span<const std::byte> bytes)
    -> Result<CollisionAsset>;
[[nodiscard]] auto LoadCollisionAssetFromCacheFile(const std::filesystem::path& path)
    -> Result<CollisionAsset>;

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_COLLISION_ASSET_H_
