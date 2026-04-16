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
            << "  list [type]          List registered processes (optional type filter)\n"
            << "  watch <target> <path> Query a watcher path from a target process\n"
            << "\n"
            << "Examples:\n"
            << "  atlas_tool list\n"
            << "  atlas_tool list baseapp\n"
            << "  atlas_tool watch baseapp-1 server/tick_rate\n";
}

static auto ParseProcessType(std::string_view name) -> std::optional<ProcessType> {
  auto r = ProcessTypeFromName(name);
  if (!r) return std::nullopt;
  return *r;
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

static auto CmdWatch(MachinedClient& client, std::string_view target_name,
                     std::string_view watcher_path) -> int {
  // We can't directly send a WatcherRequest from atlas_tool since MachinedClient
  // doesn't expose a raw watcher API.  For now, print a message noting this
  // feature is implemented server-side and can be accessed from processes.
  // A future iteration will wire WatcherRequest through MachinedClient.
  std::cout << std::format("watch {} {} — watcher forwarding requires a registered process\n",
                           target_name, watcher_path);
  std::cout << "(connect a server process to machined and use its MachinedClient to query)\n";
  (void)client;
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
      std::cerr << "watch requires <target> <path>\n";
      PrintUsage();
      return 1;
    }
    return CmdWatch(client, argv[arg_idx], argv[arg_idx + 1]);
  } else {
    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage();
    return 1;
  }
}
