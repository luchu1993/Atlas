#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "physics/collision_asset.h"
#include "platform/filesystem.h"
#include "server/machined_client.h"
#include "server/server_config.h"

#ifdef ATLAS_ATLAS_TOOL_HAS_JOLT
#include "physics_jolt/jolt_init.h"
#include "physics_jolt/jolt_physics_query.h"
#endif

#ifdef ATLAS_ATLAS_TOOL_HAS_RECAST
#include "navigation_recast/recast_bake.h"
#include "navigation_recast/recast_nav_backend.h"
#endif

using namespace atlas;
using namespace atlas::machined;

static void PrintUsage() {
  std::cerr << "Usage: atlas_tool [--machined <host:port>] <command> [args]\n"
            << "\n"
            << "Commands:\n"
            << "  list [type]            List registered processes (optional type filter)\n"
            << "  watch <type[:name]> <path>\n"
            << "                         Query a watcher path on a target process\n"
            << "                         (no name = first instance of type)\n"
            << "  set-watch <type[:name]> <path> <value>\n"
            << "                         Set a read-write watcher path on a target process\n"
            << "  shutdown <type[:name]> [reason]\n"
            << "                         Forward a shutdown request via machined\n"
            << "                         (no name = all instances of type)\n"
            << "  validate_collision <path>\n"
            << "  cook_collision <input.collision.json> [-o <output.collisioncache>]\n"
            << "                         Validate an Atlas collision JSON asset\n"
            << "  dump_collision <path> --obj <path>\n"
            << "                         Write an OBJ debug preview for a collision asset\n"
            << "  recook --invalid <dir>\n"
            << "                         Re-cook .collisioncache files in <dir> whose\n"
            << "                         jolt_version_stamp or cooked blob is stale\n"
            << "  validate_nav <map.nav.json>\n"
            << "                         Validate an Atlas nav params asset\n"
            << "  cook_nav <map.collision.json> --params <map.nav.json>\n"
            << "                         Bake a navmesh in memory and print stats\n"
            << "  dump_nav <map.collision.json> --params <map.nav.json> --obj <out.obj>\n"
            << "                         Write an OBJ of the baked navmesh surface\n"
            << "  path_nav <map.collision.json> --params <map.nav.json> --from x,y,z --to x,y,z [--obj <out.obj>]\n"
            << "                         Bake, run one FindPath, print the result\n"
            << "\n"
            << "Examples:\n"
            << "  atlas_tool list\n"
            << "  atlas_tool list baseapp\n"
            << "  atlas_tool watch baseapp:baseapp-1 app/uptime_seconds\n"
            << "  atlas_tool set-watch cellappmgr cellappmgr/lb/retire/app_id 2\n"
            << "  atlas_tool watch cellapp tick/duration_ms\n"
            << "  atlas_tool shutdown baseapp:baseapp-1\n"
            << "  atlas_tool shutdown cellapp 1\n"
            << "  atlas_tool validate_collision maps/test.collision.json\n"
            << "  atlas_tool dump_collision maps/test.collision.json --obj map.obj\n"
            << "  atlas_tool cook_collision maps/test.collision.json\n"
            << "  atlas_tool cook_collision maps/test.collision.json -o maps/test.collisioncache\n"
            << "  atlas_tool recook --invalid maps/\n"
            << "  atlas_tool validate_nav maps/test.nav.json\n"
            << "  atlas_tool cook_nav maps/test.collision.json --params maps/test.nav.json\n"
            << "  atlas_tool dump_nav maps/test.collision.json --params maps/test.nav.json --obj nav.obj\n"
            << "  atlas_tool path_nav maps/test.collision.json --params maps/test.nav.json"
               " --from -8,0,-8 --to 8,0,8 --obj path.obj\n";
}

static auto ParseProcessType(std::string_view name) -> std::optional<ProcessType> {
  auto r = ProcessTypeFromName(name);
  if (!r) return std::nullopt;
  return *r;
}

struct TargetSpec {
  ProcessType type;
  std::string name;
};

static auto ParseTargetSpec(std::string_view spec) -> std::optional<TargetSpec> {
  auto colon = spec.find(':');
  std::string_view type_str = (colon == std::string_view::npos) ? spec : spec.substr(0, colon);
  std::string_view name_str =
      (colon == std::string_view::npos) ? std::string_view{} : spec.substr(colon + 1);

  auto type = ParseProcessType(type_str);
  if (!type) return std::nullopt;
  return TargetSpec{*type, std::string(name_str)};
}

template <typename Pred>
static void DrainUntil(EventDispatcher& disp, Pred pred,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
  }
}

static auto CmdList(MachinedClient& client, std::optional<ProcessType> type_filter) -> int {
  std::vector<ProcessInfo> all_processes;

  if (type_filter) {
    all_processes = client.QuerySync(*type_filter);
  } else {
    constexpr ProcessType kTypes[] = {
        ProcessType::kLoginApp, ProcessType::kBaseApp,    ProcessType::kBaseAppMgr,
        ProcessType::kCellApp,  ProcessType::kCellAppMgr, ProcessType::kDbApp,
        ProcessType::kDbAppMgr, ProcessType::kReviver,
    };
    for (auto t : kTypes) {
      auto procs = client.QuerySync(t, std::chrono::seconds(2));
      for (auto& p : procs) all_processes.push_back(std::move(p));
    }
  }

  if (all_processes.empty()) {
    std::cout << "(no processes registered)\n";
    return 0;
  }

  std::cout << std::format("{:<20} {:<20} {:<22} {:>6}  {:>5}\n", "TYPE", "NAME", "INTERNAL_ADDR",
                           "PID", "LOAD");
  std::cout << std::string(75, '-') << "\n";
  for (const auto& p : all_processes) {
    std::cout << std::format("{:<20} {:<20} {:<22} {:>6}  {:.1f}%\n",
                             ProcessTypeName(p.process_type), p.name, p.internal_addr.ToString(),
                             p.pid, p.load * 100.0f);
  }
  return 0;
}

static auto CmdWatch(EventDispatcher& dispatcher, MachinedClient& client, const TargetSpec& target,
                     std::string_view watcher_path) -> int {
  bool done = false;
  bool found = false;
  std::string source_name;
  std::string value;
  client.QueryWatcher(target.type, target.name, watcher_path,
                      [&](bool f, const std::string& src, const std::string& v) {
                        found = f;
                        source_name = src;
                        value = v;
                        done = true;
                      });

  DrainUntil(dispatcher, [&] { return done; });

  if (!done) {
    std::cerr << "watch: timeout waiting for response from machined\n";
    return 1;
  }
  if (!found) {
    std::cerr << std::format("watch: no value (target={}, path={})\n",
                             source_name.empty() ? std::string(target.name) : source_name,
                             watcher_path);
    return 1;
  }
  std::cout << std::format("{:<24} {}\n", source_name, value);
  return 0;
}

static auto CmdSetWatch(EventDispatcher& dispatcher, MachinedClient& client,
                        const TargetSpec& target, std::string_view watcher_path,
                        std::string_view watcher_value) -> int {
  bool done = false;
  bool found = false;
  std::string source_name;
  std::string value;
  client.SetWatcher(target.type, target.name, watcher_path, watcher_value,
                    [&](bool f, const std::string& src, const std::string& v) {
                      found = f;
                      source_name = src;
                      value = v;
                      done = true;
                    });

  DrainUntil(dispatcher, [&] { return done; });

  if (!done) {
    std::cerr << "set-watch: timeout waiting for response from machined\n";
    return 1;
  }
  if (!found) {
    std::cerr << std::format("set-watch: failed (target={}, path={}, value={})\n",
                             source_name.empty() ? std::string(target.name) : source_name,
                             watcher_path, watcher_value);
    return 1;
  }
  std::cout << std::format("{:<24} {}\n", source_name, value);
  return 0;
}

static auto CmdShutdown(EventDispatcher& dispatcher, MachinedClient& client,
                        const TargetSpec& target, uint8_t reason) -> int {
  client.RequestShutdownTarget(target.type, target.name, reason);
  // Drain briefly so the message actually flushes to machined.
  DrainUntil(dispatcher, [] { return false; }, std::chrono::milliseconds(200));

  if (target.name.empty()) {
    std::cout << std::format("shutdown forwarded to all {} processes (reason={})\n",
                             ProcessTypeName(target.type), reason);
  } else {
    std::cout << std::format("shutdown forwarded to {}:{} (reason={})\n",
                             ProcessTypeName(target.type), target.name, reason);
  }
  return 0;
}

static auto CmdValidateCollision(std::string_view path) -> int {
  auto asset = physics::LoadCollisionAssetFromFile(std::filesystem::path(path));
  if (!asset) {
    std::cerr << std::format("validate_collision: {}\n", asset.Error().Message());
    return 1;
  }
  std::cout << std::format(
      "collision asset ok: boxes={} planes={} spheres={} capsules={} meshes={} convexes={} "
      "heightfields={} source_hash={}\n",
      asset->boxes.size(), asset->planes.size(), asset->spheres.size(),
      asset->capsules.size(), asset->meshes.size(), asset->convexes.size(),
      asset->heightfields.size(), asset->source_hash);
  return 0;
}

static auto CmdDumpCollision(std::string_view input_path, std::string_view obj_path) -> int {
  auto asset = physics::LoadCollisionAssetFromFile(std::filesystem::path(input_path));
  if (!asset) {
    std::cerr << std::format("dump_collision: {}\n", asset.Error().Message());
    return 1;
  }
  const auto obj = physics::DumpCollisionAssetToObj(*asset);
  auto written = fs::WriteTextFile(std::filesystem::path(obj_path), obj);
  if (!written) {
    std::cerr << std::format("dump_collision: {}\n", written.Error().Message());
    return 1;
  }
  std::cout << std::format("collision OBJ written: {} boxes={} planes={}\n", obj_path,
                           asset->boxes.size(), asset->planes.size());
  return 0;
}

static auto DefaultCachePath(const std::filesystem::path& src) -> std::filesystem::path {
  auto out = src;
  out.replace_extension(".collisioncache");
  return out;
}

static auto CookSourceToCache(const std::filesystem::path& src,
                              const std::filesystem::path& out)
    -> Result<physics::LoadedCollisionCache> {
  auto json = fs::ReadTextFile(src);
  if (!json) return json.Error();

  auto bin_path = src;
  bin_path.replace_extension(".bin");
  std::vector<std::byte> bin;
  if (std::filesystem::exists(bin_path)) {
    auto bytes = fs::ReadFile(bin_path);
    if (!bytes) return bytes.Error();
    bin = std::move(*bytes);
  }

  auto asset = physics::LoadCollisionAssetFromJson(*json, bin);
  if (!asset) return asset.Error();

#ifdef ATLAS_ATLAS_TOOL_HAS_JOLT
  physics::jolt::Initialize();
  auto cooked = physics::JoltPhysicsQuery::CookCollisionMeshes(asset->meshes);
  if (!cooked) return cooked.Error();
  const uint64_t stamp = physics::JoltPhysicsQuery::CurrentJoltStamp();
  auto bytes = physics::WriteCollisionCacheBytes(*json, bin, asset->source_hash, stamp,
                                                  std::span<const std::byte>(*cooked));
#else
  // Jolt-less build cooks the envelope only; runtime that needs Jolt will refuse it.
  auto bytes = physics::WriteCollisionCacheBytes(*json, bin, asset->source_hash, 0u, {});
#endif

  if (auto wr = fs::WriteFile(out, std::span<const std::byte>(bytes)); !wr) {
    return wr.Error();
  }
  return physics::LoadCollisionCacheFromBytes(std::span<const std::byte>(bytes));
}

static auto CmdCookCollision(std::string_view input_path,
                              std::string_view output_path) -> int {
  std::filesystem::path src(input_path);
  std::filesystem::path out =
      output_path.empty() ? DefaultCachePath(src) : std::filesystem::path(output_path);

  auto loaded = CookSourceToCache(src, out);
  if (!loaded) {
    std::cerr << std::format("cook_collision: {}\n", loaded.Error().Message());
    return 1;
  }
  std::cout << std::format(
      "collision cache written: {} boxes={} planes={} spheres={} capsules={} meshes={} "
      "convexes={} heightfields={} cooked_bytes={} stamp=0x{:016x} source_hash={}\n",
      out.string(), loaded->asset.boxes.size(), loaded->asset.planes.size(),
      loaded->asset.spheres.size(), loaded->asset.capsules.size(), loaded->asset.meshes.size(),
      loaded->asset.convexes.size(), loaded->asset.heightfields.size(), loaded->cooked.size(),
      loaded->jolt_version_stamp, loaded->asset.source_hash);
  return 0;
}

// True iff the cache must be re-cooked under the current toolchain. Caller
// should already have ensured the file is a readable .collisioncache.
[[nodiscard]] static auto IsCacheStale(const physics::LoadedCollisionCache& loaded)
    -> std::optional<std::string> {
#ifdef ATLAS_ATLAS_TOOL_HAS_JOLT
  const uint64_t current = physics::JoltPhysicsQuery::CurrentJoltStamp();
  if (loaded.jolt_version_stamp != current) {
    return std::format("jolt_version_stamp 0x{:016x} != current 0x{:016x}",
                       loaded.jolt_version_stamp, current);
  }
  if (!loaded.asset.meshes.empty() && loaded.cooked.empty()) {
    return std::string{"source contains meshes but cache cooked blob is empty"};
  }
#else
  (void)loaded;
#endif
  return std::nullopt;
}

// Compares the source .json/.bin on disk to the copy embedded in the cache; a
// difference means an edit the jolt stamp alone can't see, so the cache is stale.
[[nodiscard]] static auto SourceContentChanged(std::span<const std::byte> cache_bytes,
                                               const std::filesystem::path& src)
    -> std::optional<std::string> {
  auto env = physics::ReadCollisionCacheSources(cache_bytes);
  if (!env) return std::nullopt;
  auto json = fs::ReadTextFile(src);
  if (!json) return std::nullopt;
  if (*json != env->source_json) return std::string{"source .json changed since cook"};

  auto bin_path = src;
  bin_path.replace_extension(".bin");
  std::vector<std::byte> bin;
  if (std::filesystem::exists(bin_path)) {
    auto bytes = fs::ReadFile(bin_path);
    if (bytes) bin = std::move(*bytes);
  }
  if (bin != env->source_bin) return std::string{"source .bin changed since cook"};
  return std::nullopt;
}

static auto CmdRecookInvalid(std::string_view dir_path) -> int {
  std::filesystem::path dir(dir_path);
  if (!std::filesystem::is_directory(dir)) {
    std::cerr << std::format("recook: {} is not a directory\n", dir.string());
    return 1;
  }
#ifdef ATLAS_ATLAS_TOOL_HAS_JOLT
  physics::jolt::Initialize();
#endif

  std::size_t scanned = 0;
  std::size_t stale = 0;
  std::size_t recooked = 0;
  std::size_t failed = 0;
  for (auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".collisioncache") continue;
    ++scanned;

    auto bytes = fs::ReadFile(entry.path());
    if (!bytes) {
      ++failed;
      std::cerr << std::format("recook: {} unreadable: {}\n", entry.path().string(),
                                bytes.Error().Message());
      continue;
    }
    auto loaded = physics::LoadCollisionCacheFromBytes(std::span<const std::byte>(*bytes));
    if (!loaded) {
      ++failed;
      std::cerr << std::format("recook: {} unreadable: {}\n", entry.path().string(),
                                loaded.Error().Message());
      continue;
    }

    // map.collision.collisioncache → map.collision.json
    auto src = entry.path();
    src.replace_extension(".json");
    auto reason = IsCacheStale(*loaded);
    if (!reason && std::filesystem::exists(src)) {
      reason = SourceContentChanged(std::span<const std::byte>(*bytes), src);
    }
    if (!reason) continue;
    ++stale;

    if (!std::filesystem::exists(src)) {
      ++failed;
      std::cerr << std::format("recook: {} stale ({}) but source {} missing\n",
                                entry.path().string(), *reason, src.string());
      continue;
    }

    auto re = CookSourceToCache(src, entry.path());
    if (!re) {
      ++failed;
      std::cerr << std::format("recook: {} cook failed: {}\n", entry.path().string(),
                                re.Error().Message());
      continue;
    }
    ++recooked;
    std::cout << std::format("recook: {} ({})\n", entry.path().string(), *reason);
  }
  std::cout << std::format(
      "recook summary: scanned={} stale={} recooked={} failed={}\n", scanned, stale,
      recooked, failed);
  return failed == 0 ? 0 : 1;
}

static auto FindFlag(int argc, char** argv, int from, std::string_view flag) -> std::string_view {
  for (int i = from; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == flag) return argv[i + 1];
  }
  return {};
}

static auto CmdValidateNav(std::string_view path) -> int {
  auto params = nav::LoadNavParamsFromFile(std::filesystem::path(path));
  if (!params) {
    std::cerr << std::format("validate_nav: {}\n", params.Error().Message());
    return 1;
  }
  std::cout << std::format(
      "nav params ok: agent(r={:.2f} h={:.2f} climb={:.2f} slope={:.0f}) cell({:.2f},{:.2f}) "
      "overrides={} source_hash={}\n",
      params->bake.agent_radius_m, params->bake.agent_height_m, params->bake.agent_max_climb_m,
      params->bake.agent_max_slope_deg, params->bake.cell_size_m, params->bake.cell_height_m,
      params->overrides.size(), params->source_hash);
  return 0;
}

#ifdef ATLAS_ATLAS_TOOL_HAS_RECAST
[[nodiscard]] static auto ParseVec3(std::string_view s, math::Vector3& out) -> bool {
  float v[3] = {0.0f, 0.0f, 0.0f};
  std::size_t start = 0;
  for (int i = 0; i < 3; ++i) {
    const std::size_t comma = s.find(',', start);
    if (i < 2 && comma == std::string_view::npos) return false;
    const std::string_view tok = (i < 2) ? s.substr(start, comma - start) : s.substr(start);
    if (std::from_chars(tok.data(), tok.data() + tok.size(), v[i]).ec != std::errc{}) return false;
    start = (comma == std::string_view::npos) ? s.size() : comma + 1;
  }
  out = math::Vector3{v[0], v[1], v[2]};
  return true;
}

// Loads the collision asset + nav params and derives nav input (pure
// atlas_navigation); the bake that follows is the recast-gated step.
static auto LoadNavDerived(std::string_view collision_path, std::string_view params_path,
                           nav::NavParams& params_out, nav::NavDeriveResult& derived_out) -> int {
  auto asset = physics::LoadCollisionAssetFromFile(std::filesystem::path(collision_path));
  if (!asset) {
    std::cerr << std::format("nav: {}\n", asset.Error().Message());
    return 1;
  }
  auto params = nav::LoadNavParamsFromFile(std::filesystem::path(params_path));
  if (!params) {
    std::cerr << std::format("nav: {}\n", params.Error().Message());
    return 1;
  }
  params_out = std::move(*params);
  derived_out = nav::DeriveNavInput(*asset, params_out);
  for (const auto& warning : derived_out.stats.warnings) {
    std::cerr << std::format("nav: warn: {}\n", warning);
  }
  return 0;
}

static auto CmdCookNav(std::string_view collision_path, std::string_view params_path) -> int {
  nav::NavParams params;
  nav::NavDeriveResult derived;
  if (int rc = LoadNavDerived(collision_path, params_path, params, derived); rc != 0) return rc;
  auto baked = nav::BuildNavMeshData(derived.geometry, params.bake);
  if (!baked) {
    std::cerr << std::format("cook_nav: {}\n", baked.Error().Message());
    return 1;
  }
  const auto& r = baked->report;
  std::cout << std::format(
      "navmesh baked: input_tris={} polys={} verts={} walkable_area={:.1f} m^2 "
      "(skipped convex={} sphere={} capsule={} plane={})\n",
      r.input_triangles, r.poly_count, r.poly_vertex_count, r.walkable_area_m2,
      derived.stats.skipped_convexes, derived.stats.skipped_spheres,
      derived.stats.skipped_capsules, derived.stats.skipped_planes);
  nav::FreeNavMeshData(*baked);
  return 0;
}

static auto CmdDumpNav(std::string_view collision_path, std::string_view params_path,
                       std::string_view obj_path) -> int {
  nav::NavParams params;
  nav::NavDeriveResult derived;
  if (int rc = LoadNavDerived(collision_path, params_path, params, derived); rc != 0) return rc;
  auto mesh = nav::BuildNavDebugMesh(derived.geometry, params.bake);
  if (!mesh) {
    std::cerr << std::format("dump_nav: {}\n", mesh.Error().Message());
    return 1;
  }
  std::string out = "# Atlas navmesh debug dump\n";
  out += std::format("# polys {} verts {} walkable_area {:.1f}\n", mesh->report.poly_count,
                     mesh->report.poly_vertex_count, mesh->report.walkable_area_m2);
  for (const auto& v : mesh->vertices) {
    out += std::format("v {:.6g} {:.6g} {:.6g}\n", v.x, v.y, v.z);
  }
  for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
    out += std::format("f {} {} {}\n", mesh->indices[i] + 1, mesh->indices[i + 1] + 1,
                       mesh->indices[i + 2] + 1);
  }
  if (auto wr = fs::WriteTextFile(std::filesystem::path(obj_path), out); !wr) {
    std::cerr << std::format("dump_nav: {}\n", wr.Error().Message());
    return 1;
  }
  std::cout << std::format("nav OBJ written: {} polys={} walkable_area={:.1f}\n", obj_path,
                           mesh->report.poly_count, mesh->report.walkable_area_m2);
  return 0;
}

static auto CmdPathNav(std::string_view collision_path, std::string_view params_path,
                       const math::Vector3& from, const math::Vector3& to,
                       std::string_view obj_path) -> int {
  nav::NavParams params;
  nav::NavDeriveResult derived;
  if (int rc = LoadNavDerived(collision_path, params_path, params, derived); rc != 0) return rc;
  const nav::RecastNavBackendFactory backend;
  auto query = backend.Bake(derived.geometry, params.bake);
  if (!query) {
    std::cerr << std::format("path_nav: {}\n", query.Error().Message());
    return 1;
  }
  const nav::NavQueryFilter filter;
  const auto path = (*query)->FindPath(from, to, filter);
  const char* status = path.status == nav::NavPathStatus::kReached    ? "reached"
                       : path.status == nav::NavPathStatus::kPartial ? "partial"
                                                                     : "empty";
  std::cout << std::format("path: status={} waypoints={} length={:.2f} m\n", status,
                           path.waypoints.size(), path.length_m);
  if (!obj_path.empty() && !path.waypoints.empty()) {
    std::string out = "# Atlas nav path\n";
    for (const auto& w : path.waypoints) {
      out += std::format("v {:.6g} {:.6g} {:.6g}\n", w.x, w.y, w.z);
    }
    out += "l";
    for (std::size_t i = 0; i < path.waypoints.size(); ++i) out += std::format(" {}", i + 1);
    out += "\n";
    if (auto wr = fs::WriteTextFile(std::filesystem::path(obj_path), out); !wr) {
      std::cerr << std::format("path_nav: {}\n", wr.Error().Message());
      return 1;
    }
    std::cout << std::format("path OBJ written: {}\n", obj_path);
  }
  return 0;
}
#endif  // ATLAS_ATLAS_TOOL_HAS_RECAST

int main(int argc, char* argv[]) {
  Address machined_addr("127.0.0.1", 20018);

  int arg_idx = 1;

  while (arg_idx < argc) {
    std::string_view arg(argv[arg_idx]);
    if (arg == "--machined" && arg_idx + 1 < argc) {
      std::string_view spec(argv[arg_idx + 1]);
      auto colon = spec.rfind(':');
      if (colon == std::string_view::npos) {
        std::cerr << "Invalid machined address (expected host:port): " << spec << "\n";
        return 1;
      }
      std::string host(spec.substr(0, colon));
      auto port_str = spec.substr(colon + 1);
      uint16_t port = static_cast<uint16_t>(std::stoul(std::string(port_str)));
      machined_addr = Address(host, port);
      arg_idx += 2;
    } else {
      break;
    }
  }

  if (arg_idx >= argc) {
    PrintUsage();
    return 1;
  }

  std::string_view command(argv[arg_idx++]);

  if (command == "validate_collision") {
    if (arg_idx >= argc) {
      std::cerr << "validate_collision requires <path>\n";
      PrintUsage();
      return 1;
    }
    return CmdValidateCollision(argv[arg_idx]);
  }

  if (command == "dump_collision") {
    if (arg_idx + 2 >= argc || std::string_view(argv[arg_idx + 1]) != "--obj") {
      std::cerr << "dump_collision requires <path> --obj <path>\n";
      PrintUsage();
      return 1;
    }
    return CmdDumpCollision(argv[arg_idx], argv[arg_idx + 2]);
  }

  if (command == "cook_collision") {
    if (arg_idx >= argc) {
      std::cerr << "cook_collision requires <input.collision.json>\n";
      PrintUsage();
      return 1;
    }
    std::string_view input = argv[arg_idx++];
    std::string_view output;
    if (arg_idx < argc && std::string_view(argv[arg_idx]) == "-o") {
      if (arg_idx + 1 >= argc) {
        std::cerr << "cook_collision: -o requires <path>\n";
        return 1;
      }
      output = argv[arg_idx + 1];
      arg_idx += 2;
    }
    if (arg_idx < argc) {
      std::cerr << "cook_collision: unexpected argument '" << argv[arg_idx] << "'\n";
      PrintUsage();
      return 1;
    }
    return CmdCookCollision(input, output);
  }

  if (command == "recook") {
    if (arg_idx + 1 >= argc || std::string_view(argv[arg_idx]) != "--invalid") {
      std::cerr << "recook requires --invalid <dir>\n";
      PrintUsage();
      return 1;
    }
    return CmdRecookInvalid(argv[arg_idx + 1]);
  }

  if (command == "validate_nav") {
    if (arg_idx >= argc) {
      std::cerr << "validate_nav requires <map.nav.json>\n";
      PrintUsage();
      return 1;
    }
    return CmdValidateNav(argv[arg_idx]);
  }

  if (command == "cook_nav" || command == "dump_nav" || command == "path_nav") {
    if (arg_idx >= argc) {
      std::cerr << command << " requires <map.collision.json> --params <map.nav.json>\n";
      PrintUsage();
      return 1;
    }
    std::string_view params = FindFlag(argc, argv, arg_idx, "--params");
    if (params.empty()) {
      std::cerr << command << " requires --params <map.nav.json>\n";
      return 1;
    }
#ifdef ATLAS_ATLAS_TOOL_HAS_RECAST
    std::string_view collision = argv[arg_idx];
    if (command == "cook_nav") return CmdCookNav(collision, params);
    if (command == "dump_nav") {
      std::string_view obj = FindFlag(argc, argv, arg_idx, "--obj");
      if (obj.empty()) {
        std::cerr << "dump_nav requires --obj <out.obj>\n";
        return 1;
      }
      return CmdDumpNav(collision, params, obj);
    }
    std::string_view from_s = FindFlag(argc, argv, arg_idx, "--from");
    std::string_view to_s = FindFlag(argc, argv, arg_idx, "--to");
    math::Vector3 from;
    math::Vector3 to;
    if (from_s.empty() || to_s.empty() || !ParseVec3(from_s, from) || !ParseVec3(to_s, to)) {
      std::cerr << "path_nav requires --from x,y,z --to x,y,z\n";
      return 1;
    }
    return CmdPathNav(collision, params, from, to, FindFlag(argc, argv, arg_idx, "--obj"));
#else
    std::cerr << command << " requires atlas_tool built with ATLAS_ENABLE_RECAST\n";
    return 1;
#endif
  }

  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);

  MachinedClient client(dispatcher, network);

  if (!client.Connect(machined_addr)) {
    std::cerr << "Failed to connect to machined at " << machined_addr.ToString() << "\n";
    return 1;
  }

  dispatcher.ProcessOnce();

  if (command == "list") {
    std::optional<ProcessType> type_filter;
    if (arg_idx < argc) {
      type_filter = ParseProcessType(argv[arg_idx]);
      if (!type_filter) {
        std::cerr << "Unknown process type: " << argv[arg_idx] << "\n";
        return 1;
      }
    }
    return CmdList(client, type_filter);
  } else if (command == "watch") {
    if (arg_idx + 1 >= argc) {
      std::cerr << "watch requires <type[:name]> <path>\n";
      PrintUsage();
      return 1;
    }
    auto target = ParseTargetSpec(argv[arg_idx]);
    if (!target) {
      std::cerr << "watch: bad target spec: " << argv[arg_idx] << "\n";
      return 1;
    }
    return CmdWatch(dispatcher, client, *target, argv[arg_idx + 1]);
  } else if (command == "set-watch") {
    if (arg_idx + 2 >= argc) {
      std::cerr << "set-watch requires <type[:name]> <path> <value>\n";
      PrintUsage();
      return 1;
    }
    auto target = ParseTargetSpec(argv[arg_idx]);
    if (!target) {
      std::cerr << "set-watch: bad target spec: " << argv[arg_idx] << "\n";
      return 1;
    }
    return CmdSetWatch(dispatcher, client, *target, argv[arg_idx + 1], argv[arg_idx + 2]);
  } else if (command == "shutdown") {
    if (arg_idx >= argc) {
      std::cerr << "shutdown requires <type[:name]>\n";
      PrintUsage();
      return 1;
    }
    auto target = ParseTargetSpec(argv[arg_idx]);
    if (!target) {
      std::cerr << "shutdown: bad target spec: " << argv[arg_idx] << "\n";
      return 1;
    }
    uint8_t reason = 0;
    if (arg_idx + 1 < argc) {
      reason = static_cast<uint8_t>(std::stoul(argv[arg_idx + 1]));
    }
    return CmdShutdown(dispatcher, client, *target, reason);
  } else {
    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage();
    return 1;
  }
}
