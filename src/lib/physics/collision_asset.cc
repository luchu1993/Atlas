#include "physics/collision_asset.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 5054)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>

#include "platform/filesystem.h"

namespace atlas::physics {
namespace {

constexpr float kEpsilon = 1e-5f;
constexpr float kDebugPlaneHalfExtentM = 10.0f;

[[nodiscard]] auto Invalid(std::string message) -> Error {
  return Error{ErrorCode::kInvalidArgument, std::move(message)};
}

void AppendVertex(std::string& out, const math::Vector3& v) {
  out += std::format("v {:.6g} {:.6g} {:.6g}\n", v.x, v.y, v.z);
}

void AppendFace(std::string& out, std::size_t a, std::size_t b, std::size_t c,
                std::size_t d) {
  out += std::format("f {} {} {} {}\n", a, b, c, d);
}

void AppendBoxObj(std::string& out, const StaticBox& box, std::size_t index,
                  std::size_t& next_vertex) {
  out += std::format("g box_{}_layer_{}\n", index, box.layer);
  const auto base = next_vertex;
  const std::array<math::Vector3, 8> vertices = {
      math::Vector3{box.min.x, box.min.y, box.min.z},
      math::Vector3{box.max.x, box.min.y, box.min.z},
      math::Vector3{box.max.x, box.max.y, box.min.z},
      math::Vector3{box.min.x, box.max.y, box.min.z},
      math::Vector3{box.min.x, box.min.y, box.max.z},
      math::Vector3{box.max.x, box.min.y, box.max.z},
      math::Vector3{box.max.x, box.max.y, box.max.z},
      math::Vector3{box.min.x, box.max.y, box.max.z},
  };
  for (const auto& vertex : vertices) AppendVertex(out, vertex);
  AppendFace(out, base + 0, base + 1, base + 2, base + 3);
  AppendFace(out, base + 4, base + 7, base + 6, base + 5);
  AppendFace(out, base + 0, base + 4, base + 5, base + 1);
  AppendFace(out, base + 3, base + 2, base + 6, base + 7);
  AppendFace(out, base + 0, base + 3, base + 7, base + 4);
  AppendFace(out, base + 1, base + 5, base + 6, base + 2);
  next_vertex += vertices.size();
}

void AppendPlaneObj(std::string& out, const StaticPlane& plane, std::size_t index,
                    std::size_t& next_vertex) {
  out += std::format("g plane_{}_layer_{}\n", index, plane.layer);
  const auto normal = plane.normal.Normalized();
  const auto helper = std::fabs(normal.y) > 0.9f ? math::Vector3::UnitX()
                                                 : math::Vector3::UnitY();
  const auto tangent = helper.Cross(normal).Normalized();
  const auto bitangent = normal.Cross(tangent).Normalized();
  const auto u = tangent * kDebugPlaneHalfExtentM;
  const auto v = bitangent * kDebugPlaneHalfExtentM;
  const auto base = next_vertex;
  AppendVertex(out, plane.point - u - v);
  AppendVertex(out, plane.point + u - v);
  AppendVertex(out, plane.point + u + v);
  AppendVertex(out, plane.point - u + v);
  AppendFace(out, base + 0, base + 1, base + 2, base + 3);
  next_vertex += 4;
}

[[nodiscard]] auto MemberPath(std::string_view object_path, std::string_view member)
    -> std::string {
  return std::format("{}.{}", object_path, member);
}

[[nodiscard]] auto RequiredMember(const rapidjson::Value& value, std::string_view object_path,
                                  std::string_view member) -> Result<const rapidjson::Value*> {
  if (!value.IsObject()) {
    return Invalid(std::format("{} must be an object", object_path));
  }
  const std::string member_name(member);
  auto it = value.FindMember(member_name.c_str());
  if (it == value.MemberEnd()) {
    return Invalid(std::format("{} is required", MemberPath(object_path, member)));
  }
  return &it->value;
}

[[nodiscard]] auto RequiredString(const rapidjson::Value& value, std::string_view object_path,
                                  std::string_view member) -> Result<std::string> {
  auto result = RequiredMember(value, object_path, member);
  if (!result) return result.Error();
  const auto* member_value = *result;
  if (!member_value->IsString()) {
    return Invalid(std::format("{} must be a string", MemberPath(object_path, member)));
  }
  return std::string(member_value->GetString(), member_value->GetStringLength());
}

[[nodiscard]] auto RequiredUint(const rapidjson::Value& value, std::string_view object_path,
                                std::string_view member) -> Result<uint32_t> {
  auto result = RequiredMember(value, object_path, member);
  if (!result) return result.Error();
  const auto* member_value = *result;
  if (!member_value->IsUint()) {
    return Invalid(std::format("{} must be an unsigned integer",
                               MemberPath(object_path, member)));
  }
  return member_value->GetUint();
}

[[nodiscard]] auto RequiredVector3(const rapidjson::Value& value, std::string_view object_path,
                                   std::string_view member) -> Result<math::Vector3> {
  auto result = RequiredMember(value, object_path, member);
  if (!result) return result.Error();
  const auto* array = *result;
  if (!array->IsArray() || array->Size() != 3) {
    return Invalid(std::format("{} must be a 3-number array",
                               MemberPath(object_path, member)));
  }

  math::Vector3 out;
  for (rapidjson::SizeType i = 0; i < 3; ++i) {
    if (!(*array)[i].IsNumber()) {
      return Invalid(std::format("{}[{}] must be a number",
                                 MemberPath(object_path, member), i));
    }
    const double v = (*array)[i].GetDouble();
    if (!std::isfinite(v)) {
      return Invalid(std::format("{}[{}] must be finite",
                                 MemberPath(object_path, member), i));
    }
    out[static_cast<std::size_t>(i)] = static_cast<float>(v);
  }
  return out;
}

[[nodiscard]] auto RequiredLayer(const rapidjson::Value& value, std::string_view object_path)
    -> Result<ObjectLayer> {
  auto layer = RequiredUint(value, object_path, "layer");
  if (!layer) return layer.Error();
  if (*layer >= 32) {
    return Invalid(std::format("{}.layer must be in [0, 31]", object_path));
  }
  return static_cast<ObjectLayer>(*layer);
}

[[nodiscard]] auto ParseBox(const rapidjson::Value& value, std::string_view path)
    -> Result<StaticBox> {
  auto min = RequiredVector3(value, path, "min");
  if (!min) return min.Error();
  auto max = RequiredVector3(value, path, "max");
  if (!max) return max.Error();
  auto layer = RequiredLayer(value, path);
  if (!layer) return layer.Error();
  if (max->x <= min->x || max->y <= min->y || max->z <= min->z) {
    return Invalid(std::format("{}.max must be greater than min on every axis", path));
  }
  return StaticBox{*min, *max, *layer};
}

[[nodiscard]] auto ParsePlane(const rapidjson::Value& value, std::string_view path)
    -> Result<StaticPlane> {
  auto point = RequiredVector3(value, path, "point");
  if (!point) return point.Error();
  auto normal = RequiredVector3(value, path, "normal");
  if (!normal) return normal.Error();
  auto layer = RequiredLayer(value, path);
  if (!layer) return layer.Error();
  if (normal->LengthSquared() <= kEpsilon * kEpsilon ||
      std::fabs(normal->y) <= kEpsilon) {
    return Invalid(std::format("{}.normal must be non-zero with a Y component", path));
  }
  if (normal->y < 0.0f) *normal = *normal * -1.0f;
  return StaticPlane{*point, *normal, *layer};
}

[[nodiscard]] auto ParseMesh(const rapidjson::Value& object, std::string_view path,
                              std::span<const std::byte> mesh_buffer)
    -> Result<MeshGeometry> {
  auto layer = RequiredLayer(object, path);
  if (!layer) return layer.Error();
  auto vbo = RequiredUint(object, path, "vertex_byte_offset");
  if (!vbo) return vbo.Error();
  auto vcount = RequiredUint(object, path, "vertex_count");
  if (!vcount) return vcount.Error();
  auto ibo = RequiredUint(object, path, "index_byte_offset");
  if (!ibo) return ibo.Error();
  auto icount = RequiredUint(object, path, "index_count");
  if (!icount) return icount.Error();

  if (*vcount == 0 || *icount == 0 || (*icount % 3) != 0) {
    return Invalid(std::format(
        "{}: vertex_count={} index_count={} (index_count must be a non-zero multiple of 3)",
        path, *vcount, *icount));
  }
  if ((*vbo % alignof(float)) != 0 || (*ibo % alignof(uint32_t)) != 0) {
    return Invalid(std::format("{}: vertex/index offsets must be 4-byte aligned", path));
  }
  const std::size_t vbytes = std::size_t{*vcount} * sizeof(float) * 3;
  const std::size_t ibytes = std::size_t{*icount} * sizeof(uint32_t);
  if (*vbo + vbytes > mesh_buffer.size() || *ibo + ibytes > mesh_buffer.size()) {
    return Invalid(std::format(
        "{}: vertex/index window exceeds mesh buffer size ({} bytes)", path,
        mesh_buffer.size()));
  }

  MeshGeometry mesh;
  mesh.layer = *layer;
  mesh.vertices.resize(*vcount);
  std::memcpy(mesh.vertices.data(), mesh_buffer.data() + *vbo, vbytes);
  mesh.indices.resize(*icount);
  std::memcpy(mesh.indices.data(), mesh_buffer.data() + *ibo, ibytes);
  for (auto idx : mesh.indices) {
    if (idx >= *vcount) {
      return Invalid(std::format("{}: index {} >= vertex_count {}", path, idx, *vcount));
    }
  }
  for (const auto& v : mesh.vertices) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
      return Invalid(std::format("{}: non-finite vertex", path));
    }
  }
  return mesh;
}

[[nodiscard]] auto ParseObjects(const rapidjson::Value& root, CollisionAsset& asset,
                                std::span<const std::byte> mesh_buffer)
    -> Result<void> {
  auto objects_result = RequiredMember(root, "root", "objects");
  if (!objects_result) return objects_result.Error();
  const auto* objects = *objects_result;
  if (!objects->IsArray()) return Invalid("root.objects must be an array");

  for (rapidjson::SizeType i = 0; i < objects->Size(); ++i) {
    const auto& object = (*objects)[i];
    const auto path = std::format("root.objects[{}]", i);
    auto shape = RequiredString(object, path, "shape");
    if (!shape) return shape.Error();
    if (*shape == "box") {
      auto box = ParseBox(object, path);
      if (!box) return box.Error();
      asset.boxes.push_back(*box);
    } else if (*shape == "plane") {
      auto plane = ParsePlane(object, path);
      if (!plane) return plane.Error();
      asset.planes.push_back(*plane);
    } else if (*shape == "mesh") {
      if (mesh_buffer.empty()) {
        return Invalid(std::format(
            "{}: mesh objects require a side-car .bin buffer; pass it to "
            "LoadCollisionAssetFromJson or use LoadCollisionAssetFromFile",
            path));
      }
      auto mesh = ParseMesh(object, path, mesh_buffer);
      if (!mesh) return mesh.Error();
      asset.meshes.push_back(std::move(*mesh));
    } else {
      return Invalid(std::format("{}.shape unsupported: {}", path, *shape));
    }
  }
  return {};
}

[[nodiscard]] auto ValidateMeshBuffer(std::span<const std::byte> buffer) -> Result<void> {
  if (buffer.empty()) return {};
  if (buffer.size() < kCollisionMeshBufferHeaderBytes) {
    return Invalid("mesh buffer shorter than 8-byte header");
  }
  if (std::memcmp(buffer.data(), kCollisionMeshBufferMagic.data(), 4) != 0) {
    return Invalid("mesh buffer magic mismatch (expected 'ACOL')");
  }
  uint32_t version = 0;
  std::memcpy(&version, buffer.data() + 4, sizeof(version));
  if (version != kCollisionMeshBufferVersion) {
    return Invalid(std::format("mesh buffer version {} not supported (expected {})",
                               version, kCollisionMeshBufferVersion));
  }
  return {};
}

[[nodiscard]] auto LoadCollisionAssetImpl(std::string_view json,
                                          std::span<const std::byte> mesh_buffer)
    -> Result<CollisionAsset> {
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if (doc.HasParseError()) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("collision JSON parse error: {} at offset {}",
                             rapidjson::GetParseError_En(doc.GetParseError()),
                             doc.GetErrorOffset())};
  }
  if (!doc.IsObject()) return Invalid("root must be an object");

  auto version = RequiredUint(doc, "root", "version");
  if (!version) return version.Error();
  if (*version != 1u && *version != kCollisionAssetVersion) {
    return Error{ErrorCode::kNotSupported,
                 std::format("collision asset version {} is not supported", *version)};
  }

  auto coordinate_system = RequiredString(doc, "root", "coordinate_system");
  if (!coordinate_system) return coordinate_system.Error();
  if (*coordinate_system != kCollisionCoordinateSystem) {
    return Invalid(std::format("root.coordinate_system unsupported: {}", *coordinate_system));
  }

  CollisionAsset asset;
  asset.version = *version;
  asset.coordinate_system = *coordinate_system;
  auto source_hash = RequiredString(doc, "root", "source_hash");
  if (!source_hash) return source_hash.Error();
  asset.source_hash = *source_hash;

  if (auto check = ValidateMeshBuffer(mesh_buffer); !check) return check.Error();

  if (auto parsed = ParseObjects(doc, asset, mesh_buffer); !parsed) return parsed.Error();
  return asset;
}

}  // namespace

auto LoadCollisionAssetFromJson(std::string_view json) -> Result<CollisionAsset> {
  return LoadCollisionAssetImpl(json, {});
}

auto LoadCollisionAssetFromJson(std::string_view json,
                                std::span<const std::byte> mesh_buffer)
    -> Result<CollisionAsset> {
  return LoadCollisionAssetImpl(json, mesh_buffer);
}

auto LoadCollisionAssetFromFile(const std::filesystem::path& path) -> Result<CollisionAsset> {
  auto text = fs::ReadTextFile(path);
  if (!text) return text.Error();

  auto bin_path = path;
  bin_path.replace_extension(".bin");
  std::vector<std::byte> mesh_buffer;
  if (std::filesystem::exists(bin_path)) {
    auto bytes = fs::ReadFile(bin_path);
    if (!bytes) return bytes.Error();
    mesh_buffer = std::move(*bytes);
  }
  return LoadCollisionAssetImpl(*text, mesh_buffer);
}

namespace {

void AppendBytes(std::vector<std::byte>& out, const void* src, std::size_t n) {
  const auto* p = static_cast<const std::byte*>(src);
  out.insert(out.end(), p, p + n);
}

void AppendUint32(std::vector<std::byte>& out, uint32_t v) {
  AppendBytes(out, &v, sizeof(v));
}

[[nodiscard]] auto ReadUint32(std::span<const std::byte> bytes, std::size_t& cursor,
                              std::string_view what) -> Result<uint32_t> {
  if (cursor + sizeof(uint32_t) > bytes.size()) {
    return Invalid(std::format("cache: truncated reading {}", what));
  }
  uint32_t v = 0;
  std::memcpy(&v, bytes.data() + cursor, sizeof(v));
  cursor += sizeof(v);
  return v;
}

}  // namespace

auto WriteCollisionCacheBytes(std::string_view source_json,
                              std::span<const std::byte> source_bin,
                              std::string_view source_hash) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(kCollisionCacheHeaderBytes + source_hash.size() + sizeof(uint32_t) +
              source_json.size() + sizeof(uint32_t) + source_bin.size());
  AppendBytes(out, kCollisionCacheMagic.data(), 4);
  AppendUint32(out, kCollisionCacheVersion);
  AppendUint32(out, 0);  // jolt_version_stamp reserved; see physics_architecture.md §4.2
  AppendUint32(out, static_cast<uint32_t>(source_hash.size()));
  AppendBytes(out, source_hash.data(), source_hash.size());
  AppendUint32(out, static_cast<uint32_t>(source_json.size()));
  AppendBytes(out, source_json.data(), source_json.size());
  AppendUint32(out, static_cast<uint32_t>(source_bin.size()));
  if (!source_bin.empty()) AppendBytes(out, source_bin.data(), source_bin.size());
  return out;
}

auto WriteCollisionCacheToFile(const std::filesystem::path& source_json_path,
                                const std::filesystem::path& cache_path) -> Result<void> {
  auto json = fs::ReadTextFile(source_json_path);
  if (!json) return json.Error();

  auto bin_path = source_json_path;
  bin_path.replace_extension(".bin");
  std::vector<std::byte> bin;
  if (std::filesystem::exists(bin_path)) {
    auto bytes = fs::ReadFile(bin_path);
    if (!bytes) return bytes.Error();
    bin = std::move(*bytes);
  }

  // Parse once so we know the source_hash to stamp; this also catches a
  // bad source asset at cook time rather than at load time.
  auto asset = LoadCollisionAssetImpl(*json, bin);
  if (!asset) return asset.Error();

  auto bytes = WriteCollisionCacheBytes(*json, bin, asset->source_hash);
  return fs::WriteFile(cache_path, std::span<const std::byte>(bytes));
}

auto LoadCollisionAssetFromCacheBytes(std::span<const std::byte> bytes)
    -> Result<CollisionAsset> {
  if (bytes.size() < kCollisionCacheHeaderBytes) {
    return Invalid("cache: shorter than 16-byte header");
  }
  if (std::memcmp(bytes.data(), kCollisionCacheMagic.data(), 4) != 0) {
    return Invalid("cache: magic mismatch (expected 'ACAC')");
  }
  std::size_t cursor = 4;
  auto cache_version = ReadUint32(bytes, cursor, "cache_version");
  if (!cache_version) return cache_version.Error();
  if (*cache_version != kCollisionCacheVersion) {
    return Error{ErrorCode::kNotSupported,
                 std::format("cache: version {} not supported (expected {}), re-cook",
                             *cache_version, kCollisionCacheVersion)};
  }
  // jolt_version_stamp is reserved; any value loads today.
  auto jolt_stamp = ReadUint32(bytes, cursor, "jolt_version_stamp");
  if (!jolt_stamp) return jolt_stamp.Error();

  auto hash_len = ReadUint32(bytes, cursor, "source_hash_len");
  if (!hash_len) return hash_len.Error();
  if (cursor + *hash_len > bytes.size()) {
    return Invalid("cache: truncated reading source_hash");
  }
  std::string source_hash(reinterpret_cast<const char*>(bytes.data() + cursor), *hash_len);
  cursor += *hash_len;

  auto json_len = ReadUint32(bytes, cursor, "json_len");
  if (!json_len) return json_len.Error();
  if (cursor + *json_len > bytes.size()) {
    return Invalid("cache: truncated reading json bytes");
  }
  std::string_view json_view(reinterpret_cast<const char*>(bytes.data() + cursor), *json_len);
  cursor += *json_len;

  auto bin_len = ReadUint32(bytes, cursor, "bin_len");
  if (!bin_len) return bin_len.Error();
  if (cursor + *bin_len > bytes.size()) {
    return Invalid("cache: truncated reading bin bytes");
  }
  std::span<const std::byte> bin_view(bytes.data() + cursor, *bin_len);
  cursor += *bin_len;

  auto asset = LoadCollisionAssetImpl(json_view, bin_view);
  if (!asset) return asset.Error();
  if (asset->source_hash != source_hash) {
    return Invalid(std::format(
        "cache: header source_hash '{}' differs from JSON source_hash '{}' — "
        "cache is corrupt",
        source_hash, asset->source_hash));
  }
  return asset;
}

auto LoadCollisionAssetFromCacheFile(const std::filesystem::path& path)
    -> Result<CollisionAsset> {
  auto bytes = fs::ReadFile(path);
  if (!bytes) return bytes.Error();
  return LoadCollisionAssetFromCacheBytes(std::span<const std::byte>(*bytes));
}

auto BuildStaticPhysicsQueryFromAsset(const CollisionAsset& asset)
    -> std::unique_ptr<StaticPhysicsQuery> {
  auto query = std::make_unique<StaticPhysicsQuery>(StaticGroundMode::kDisabled);
  for (const auto& box : asset.boxes) query->AddBox(box);
  for (const auto& plane : asset.planes) query->AddPlane(plane);
  return query;
}

auto DumpCollisionAssetToObj(const CollisionAsset& asset) -> std::string {
  std::string out;
  out += "# Atlas collision asset debug dump\n";
  out += std::format("# source_hash {}\n", asset.source_hash);
  out += std::format("# boxes {} planes {} meshes {}\n", asset.boxes.size(),
                    asset.planes.size(), asset.meshes.size());
  out += "o collision_asset\n";
  std::size_t next_vertex = 1;
  for (std::size_t i = 0; i < asset.boxes.size(); ++i) {
    AppendBoxObj(out, asset.boxes[i], i, next_vertex);
  }
  for (std::size_t i = 0; i < asset.planes.size(); ++i) {
    AppendPlaneObj(out, asset.planes[i], i, next_vertex);
  }
  for (std::size_t mi = 0; mi < asset.meshes.size(); ++mi) {
    const auto& mesh = asset.meshes[mi];
    out += std::format("g mesh_{}_layer_{}\n", mi, mesh.layer);
    const auto base = next_vertex;
    for (const auto& v : mesh.vertices) AppendVertex(out, v);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      out += std::format("f {} {} {}\n", base + mesh.indices[i], base + mesh.indices[i + 1],
                         base + mesh.indices[i + 2]);
    }
    next_vertex += mesh.vertices.size();
  }
  return out;
}

}  // namespace atlas::physics
