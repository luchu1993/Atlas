#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "physics/collision_asset.h"
#include "platform/filesystem.h"
#include "server/machined_client.h"
#include "server/server_config.h"

#ifdef ATLAS_ATLAS_TOOL_HAS_JOLT
#include "physics_jolt/jolt_init.h"
#include "physics_jolt/jolt_physics_query.h"
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
            << "  atlas_tool recook --invalid maps/\n";
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
  std::cout << std::format("collision asset ok: boxes={} planes={} source_hash={}\n",
                           asset->boxes.size(), asset->planes.size(), asset->source_hash);
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
      "collision cache written: {} boxes={} planes={} meshes={} cooked_bytes={} "
      "stamp=0x{:016x} source_hash={}\n",
      out.string(), loaded->asset.boxes.size(), loaded->asset.planes.size(),
      loaded->asset.meshes.size(), loaded->cooked.size(), loaded->jolt_version_stamp,
      loaded->asset.source_hash);
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

    auto loaded = physics::LoadCollisionCacheFromFile(entry.path());
    if (!loaded) {
      ++failed;
      std::cerr << std::format("recook: {} unreadable: {}\n", entry.path().string(),
                                loaded.Error().Message());
      continue;
    }

    auto reason = IsCacheStale(*loaded);
    if (!reason) continue;
    ++stale;

    // map.collision.collisioncache → map.collision.json
    auto src = entry.path();
    src.replace_extension(".json");
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
