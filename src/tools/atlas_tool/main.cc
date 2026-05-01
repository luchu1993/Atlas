#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "server/machined_client.h"
#include "server/server_config.h"

using namespace atlas;
using namespace atlas::machined;

// ============================================================================
// Helpers
// ============================================================================

static void PrintUsage() {
  std::cerr << "Usage: atlas_tool [--machined <host:port>] <command> [args]\n"
            << "\n"
            << "Commands:\n"
            << "  list [type]            List registered processes (optional type filter)\n"
            << "  watch <type[:name]> <path>\n"
            << "                         Query a watcher path on a target process\n"
            << "                         (no name = first instance of type)\n"
            << "  shutdown <type[:name]> [reason]\n"
            << "                         Forward a shutdown request via machined\n"
            << "                         (no name = all instances of type)\n"
            << "\n"
            << "Examples:\n"
            << "  atlas_tool list\n"
            << "  atlas_tool list baseapp\n"
            << "  atlas_tool watch baseapp:baseapp-1 app/uptime_seconds\n"
            << "  atlas_tool watch cellapp tick/duration_ms\n"
            << "  atlas_tool shutdown baseapp:baseapp-1\n"
            << "  atlas_tool shutdown cellapp 1\n";
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

// Parses "type" or "type:name".
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

// ============================================================================
// Commands
// ============================================================================

static auto CmdList(MachinedClient& client, std::optional<ProcessType> type_filter) -> int {
  // Query all types if no filter; otherwise query the specified type
  std::vector<ProcessInfo> all_processes;

  if (type_filter) {
    all_processes = client.QuerySync(*type_filter);
  } else {
    // Query each process type
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

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
  // Defaults
  Address machined_addr("127.0.0.1", 20018);

  int arg_idx = 1;

  // Parse --machined option
  while (arg_idx < argc) {
    std::string_view arg(argv[arg_idx]);
    if (arg == "--machined" && arg_idx + 1 < argc) {
      // Parse "host:port" format
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

  // Setup minimal event loop
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);

  // We need a minimal ServerConfig just to satisfy MachinedClient::send_register
  // (atlas_tool doesn't register itself, so we only connect)
  MachinedClient client(dispatcher, network);

  if (!client.Connect(machined_addr)) {
    std::cerr << "Failed to connect to machined at " << machined_addr.ToString() << "\n";
    return 1;
  }

  // Run the event loop briefly so the connection completes
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
