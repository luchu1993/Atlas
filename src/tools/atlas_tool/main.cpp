#include "network/event_dispatcher.hpp"
#include "network/machined_types.hpp"
#include "network/network_interface.hpp"
#include "server/machined_client.hpp"
#include "server/server_config.hpp"

#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

using namespace atlas;
using namespace atlas::machined;

// ============================================================================
// Helpers
// ============================================================================

static void print_usage()
{
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

static auto parse_process_type(std::string_view name) -> std::optional<ProcessType>
{
    auto r = process_type_from_name(name);
    if (!r)
        return std::nullopt;
    return *r;
}

// ============================================================================
// Commands
// ============================================================================

static auto cmd_list(MachinedClient& client, std::optional<ProcessType> type_filter) -> int
{
    // Query all types if no filter; otherwise query the specified type
    std::vector<ProcessInfo> all_processes;

    if (type_filter)
    {
        all_processes = client.query_sync(*type_filter);
    }
    else
    {
        // Query each process type
        constexpr ProcessType kTypes[] = {
            ProcessType::LoginApp, ProcessType::BaseApp,    ProcessType::BaseAppMgr,
            ProcessType::CellApp,  ProcessType::CellAppMgr, ProcessType::DBApp,
            ProcessType::DBAppMgr, ProcessType::Reviver,
        };
        for (auto t : kTypes)
        {
            auto procs = client.query_sync(t, std::chrono::seconds(2));
            for (auto& p : procs)
                all_processes.push_back(std::move(p));
        }
    }

    if (all_processes.empty())
    {
        std::cout << "(no processes registered)\n";
        return 0;
    }

    std::cout << std::format("{:<20} {:<20} {:<22} {:>6}  {:>5}\n", "TYPE", "NAME", "INTERNAL_ADDR",
                             "PID", "LOAD");
    std::cout << std::string(75, '-') << "\n";
    for (const auto& p : all_processes)
    {
        std::cout << std::format("{:<20} {:<20} {:<22} {:>6}  {:.1f}%\n",
                                 process_type_name(p.process_type), p.name,
                                 p.internal_addr.to_string(), p.pid, p.load * 100.0f);
    }
    return 0;
}

static auto cmd_watch(MachinedClient& client, std::string_view target_name,
                      std::string_view watcher_path) -> int
{
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

int main(int argc, char* argv[])
{
    // Defaults
    Address machined_addr("127.0.0.1", 20018);

    int arg_idx = 1;

    // Parse --machined option
    while (arg_idx < argc)
    {
        std::string_view arg(argv[arg_idx]);
        if (arg == "--machined" && arg_idx + 1 < argc)
        {
            // Parse "host:port" format
            std::string_view spec(argv[arg_idx + 1]);
            auto colon = spec.rfind(':');
            if (colon == std::string_view::npos)
            {
                std::cerr << "Invalid machined address (expected host:port): " << spec << "\n";
                return 1;
            }
            std::string host(spec.substr(0, colon));
            auto port_str = spec.substr(colon + 1);
            uint16_t port = static_cast<uint16_t>(std::stoul(std::string(port_str)));
            machined_addr = Address(host, port);
            arg_idx += 2;
        }
        else
        {
            break;
        }
    }

    if (arg_idx >= argc)
    {
        print_usage();
        return 1;
    }

    std::string_view command(argv[arg_idx++]);

    // Setup minimal event loop
    EventDispatcher dispatcher;
    NetworkInterface network(dispatcher);

    // We need a minimal ServerConfig just to satisfy MachinedClient::send_register
    // (atlas_tool doesn't register itself, so we only connect)
    MachinedClient client(dispatcher, network);

    if (!client.connect(machined_addr))
    {
        std::cerr << "Failed to connect to machined at " << machined_addr.to_string() << "\n";
        return 1;
    }

    // Run the event loop briefly so the connection completes
    dispatcher.process_once();

    if (command == "list")
    {
        std::optional<ProcessType> type_filter;
        if (arg_idx < argc)
        {
            type_filter = parse_process_type(argv[arg_idx]);
            if (!type_filter)
            {
                std::cerr << "Unknown process type: " << argv[arg_idx] << "\n";
                return 1;
            }
        }
        return cmd_list(client, type_filter);
    }
    else if (command == "watch")
    {
        if (arg_idx + 1 >= argc)
        {
            std::cerr << "watch requires <target> <path>\n";
            print_usage();
            return 1;
        }
        return cmd_watch(client, argv[arg_idx], argv[arg_idx + 1]);
    }
    else
    {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }
}
