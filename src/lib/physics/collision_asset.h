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

// Side-car binary layout and cooked-cache layout are documented in
// docs/physics/physics_architecture.md §4.2.
inline constexpr std::array<char, 4> kCollisionMeshBufferMagic{'A', 'C', 'O', 'L'};
inline constexpr uint32_t kCollisionMeshBufferVersion = 1;
inline constexpr std::size_t kCollisionMeshBufferHeaderBytes = 8;

inline constexpr std::array<char, 4> kCollisionCacheMagic{'A', 'C', 'A', 'C'};
inline constexpr uint32_t kCollisionCacheVersion = 2;
inline constexpr std::size_t kCollisionCacheHeaderBytes = 20;
inline constexpr float kDefaultCollisionChunkSizeM = 128.0f;
inline constexpr float kDefaultCollisionChunkBorderM = 2.0f;

struct MeshGeometry {
  std::vector<math::Vector3> vertices;
  std::vector<uint32_t> indices;  // triangle list; size % 3 == 0
  ObjectLayer layer{0};
};

// Convex hull built from a point cloud (no triangle topology). The backend
// rebuilds the hull from these points, so it needs no cooked side-channel.
struct ConvexGeometry {
  std::vector<math::Vector3> vertices;
  ObjectLayer layer{0};
};

// Square grid of samples; surface point (x,z) = origin + scale*(x,
// samples[z*sample_count+x], z). FLT_MAX sample = hole. Backend rebuilds; no cook.
struct HeightFieldGeometry {
  math::Vector3 origin{0.0f, 0.0f, 0.0f};
  math::Vector3 scale{1.0f, 1.0f, 1.0f};  // x/z = sample spacing, y = height scale
  uint32_t sample_count{0};               // samples per side; >= 4 (backend rounds up to its
                                          // block size, padding the slack with no-collision)
  std::vector<float> samples;             // row-major, sample_count * sample_count
  ObjectLayer layer{0};
};

struct CollisionAsset {
  uint32_t version{kCollisionAssetVersion};
  std::string coordinate_system{std::string(kCollisionCoordinateSystem)};
  std::string source_hash;
  std::vector<StaticBox> boxes;
  std::vector<StaticPlane> planes;
  std::vector<StaticSphere> spheres;
  std::vector<StaticCapsule> capsules;
  std::vector<MeshGeometry> meshes;
  std::vector<ConvexGeometry> convexes;
  std::vector<HeightFieldGeometry> heightfields;
};

struct LoadedCollisionCache {
  CollisionAsset asset;
  uint64_t jolt_version_stamp{0};
  std::vector<std::byte> cooked;
};

[[nodiscard]] auto LoadCollisionAssetFromJson(std::string_view json)
    -> Result<CollisionAsset>;

// Accepts assets that reference mesh data; pass the side-car bytes here.
[[nodiscard]] auto LoadCollisionAssetFromJson(std::string_view json,
                                              std::span<const std::byte> mesh_buffer)
    -> Result<CollisionAsset>;

// Reads `path` plus, if present, the side-car at `path` with extension
// swapped to .bin (e.g. foo.collision.json → foo.collision.bin).
[[nodiscard]] auto LoadCollisionAssetFromFile(const std::filesystem::path& path)
    -> Result<CollisionAsset>;

[[nodiscard]] auto BuildStaticPhysicsQueryFromAsset(const CollisionAsset& asset)
    -> std::unique_ptr<StaticPhysicsQuery>;
[[nodiscard]] auto BuildChunkedStaticPhysicsQueryFromAsset(const CollisionAsset& asset,
                                                           float chunk_size_m,
                                                           float border_m)
    -> std::unique_ptr<PhysicsQuery>;
[[nodiscard]] auto DumpCollisionAssetToObj(const CollisionAsset& asset) -> std::string;

[[nodiscard]] auto WriteCollisionCacheBytes(std::string_view source_json,
                                            std::span<const std::byte> source_bin,
                                            std::string_view source_hash,
                                            uint64_t jolt_version_stamp = 0,
                                            std::span<const std::byte> cooked = {})
    -> std::vector<std::byte>;
[[nodiscard]] auto WriteCollisionCacheToFile(const std::filesystem::path& source_json_path,
                                              const std::filesystem::path& cache_path)
    -> Result<void>;
// The source json/bin/cooked bytes a cache embeds, extracted without parsing the
// asset — lets the cook tool detect a stale cache by diffing against on-disk source.
struct CollisionCacheSources {
  uint32_t cache_version{0};
  uint64_t jolt_version_stamp{0};
  std::string source_hash;
  std::string source_json;
  std::vector<std::byte> source_bin;
  std::vector<std::byte> cooked;
};
[[nodiscard]] auto ReadCollisionCacheSources(std::span<const std::byte> bytes)
    -> Result<CollisionCacheSources>;

[[nodiscard]] auto LoadCollisionCacheFromBytes(std::span<const std::byte> bytes)
    -> Result<LoadedCollisionCache>;
[[nodiscard]] auto LoadCollisionCacheFromFile(const std::filesystem::path& path)
    -> Result<LoadedCollisionCache>;
[[nodiscard]] auto LoadCollisionAssetFromCacheBytes(std::span<const std::byte> bytes)
    -> Result<CollisionAsset>;
[[nodiscard]] auto LoadCollisionAssetFromCacheFile(const std::filesystem::path& path)
    -> Result<CollisionAsset>;

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_COLLISION_ASSET_H_
