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
[[nodiscard]] auto DumpCollisionAssetToObj(const CollisionAsset& asset) -> std::string;

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
