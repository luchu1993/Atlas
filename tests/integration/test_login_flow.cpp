#include "baseapp/baseapp_messages.hpp"
#include "loginapp/login_messages.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"
#include "network/reliable_udp.hpp"
#include "network/socket.hpp"
#include "server/machined_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace atlas;

namespace
{

template <typename Pred>
bool poll_until(EventDispatcher& disp, Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        disp.process_once();
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

auto reserve_udp_port() -> uint16_t
{
    auto sock = Socket::create_udp();
    EXPECT_TRUE(sock.has_value());
    EXPECT_TRUE(sock->bind(Address("127.0.0.1", 0)).has_value());
    auto local = sock->local_address();
    EXPECT_TRUE(local.has_value());
    return local ? local->port() : 0;
}

auto reserve_tcp_port() -> uint16_t
{
    auto sock = Socket::create_tcp();
    EXPECT_TRUE(sock.has_value());
    EXPECT_TRUE(sock->bind(Address("127.0.0.1", 0)).has_value());
    auto local = sock->local_address();
    EXPECT_TRUE(local.has_value());
    return local ? local->port() : 0;
}

#if defined(_WIN32)
auto executable_path() -> std::filesystem::path
{
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD len =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(len);
    return std::filesystem::path(buffer);
}

auto build_root() -> std::filesystem::path
{
    auto current = executable_path().parent_path();
    for (int i = 0; i < 8 && !current.empty(); ++i)
    {
        if (std::filesystem::exists(current / "csharp") && std::filesystem::exists(current / "bin"))
        {
            return current;
        }
        current = current.parent_path();
    }
    return {};
}

auto repo_root() -> std::filesystem::path
{
    auto current = build_root();
    for (int i = 0; i < 8 && !current.empty(); ++i)
    {
        if (std::filesystem::exists(current / "src" / "server" / "baseapp" / "baseapp.cpp") &&
            std::filesystem::exists(current / "tests" / "csharp" / "Atlas.Runtime.Tests" /
                                    "Atlas.Runtime.Tests.csproj"))
        {
            return current;
        }
        current = current.parent_path();
    }
    return {};
}

auto server_bin_dir() -> std::filesystem::path
{
    return build_root() / "bin" / "Debug";
}

auto runtime_config_path() -> std::filesystem::path
{
    return repo_root() / "tests" / "csharp" / "Atlas.Runtime.Tests" / "bin" / "Debug" / "net9.0" /
           "Atlas.Tests.runtimeconfig.json";
}

auto runtime_assembly_path() -> std::filesystem::path
{
    return build_root() / "csharp" / "src" / "csharp" / "Atlas.Runtime" / "Atlas.Runtime.dll";
}

auto quote_arg(const std::wstring& arg) -> std::wstring
{
    std::wstring quoted = L"\"";
    for (wchar_t ch : arg)
    {
        if (ch == L'"')
            quoted += L'\\';
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

struct ChildProcess
{
    PROCESS_INFORMATION pi{};
    std::filesystem::path log_path;
    std::string label;
    std::wstring command_line_snapshot;

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    auto operator=(const ChildProcess&) -> ChildProcess& = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : pi(other.pi),
          log_path(std::move(other.log_path)),
          label(std::move(other.label)),
          command_line_snapshot(std::move(other.command_line_snapshot))
    {
        other.pi = {};
    }

    auto operator=(ChildProcess&& other) noexcept -> ChildProcess&
    {
        if (this == &other)
            return *this;
        terminate();
        close_handles();
        pi = other.pi;
        log_path = std::move(other.log_path);
        label = std::move(other.label);
        command_line_snapshot = std::move(other.command_line_snapshot);
        other.pi = {};
        return *this;
    }

    ~ChildProcess()
    {
        terminate();
        close_handles();
    }

    static auto launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                       const std::filesystem::path& workdir, const std::string& proc_label)
        -> ChildProcess
    {
        std::wstring command_line = quote_arg(exe.wstring());
        for (const auto& arg : args)
        {
            command_line += L' ';
            command_line += quote_arg(arg);
        }

        auto log_file = std::filesystem::temp_directory_path() /
                        std::format("atlas_login_flow_{}.log", proc_label);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hLog = ::CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        if (hLog != INVALID_HANDLE_VALUE)
        {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hLog;
            si.hStdError = hLog;
            si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        }

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
        buffer.push_back(L'\0');

        const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), buffer.data(), nullptr, nullptr,
                                         hLog != INVALID_HANDLE_VALUE ? TRUE : FALSE, 0, nullptr,
                                         workdir.wstring().c_str(), &si, &pi);
        EXPECT_TRUE(ok) << "CreateProcessW failed for " << exe.string();

        if (hLog != INVALID_HANDLE_VALUE)
            ::CloseHandle(hLog);

        ChildProcess child;
        child.pi = pi;
        child.log_path = log_file;
        child.label = proc_label;
        child.command_line_snapshot = command_line;
        return child;
    }

    [[nodiscard]] auto is_running() const -> bool
    {
        if (pi.hProcess == nullptr)
            return false;
        auto code = exit_code();
        return code.has_value() && *code == STILL_ACTIVE;
    }

    [[nodiscard]] auto exit_code() const -> std::optional<DWORD>
    {
        if (pi.hProcess == nullptr)
            return std::nullopt;
        DWORD code = 0;
        if (!::GetExitCodeProcess(pi.hProcess, &code))
            return std::nullopt;
        return code;
    }

    [[nodiscard]] auto read_output_tail(std::size_t max_lines = 40) const -> std::string
    {
        if (log_path.empty() || !std::filesystem::exists(log_path))
            return "(no log file)";

        std::ifstream f(log_path, std::ios::in);
        if (!f.is_open())
            return "(cannot open log file)";

        std::deque<std::string> ring;
        std::string line;
        while (std::getline(f, line))
        {
            ring.push_back(std::move(line));
            if (ring.size() > max_lines)
                ring.pop_front();
        }

        if (ring.empty())
            return "(empty)";

        std::ostringstream oss;
        for (const auto& l : ring)
            oss << "  " << l << "\n";
        return oss.str();
    }

    [[nodiscard]] auto diagnostic() const -> std::string
    {
        auto code = exit_code();
        std::ostringstream oss;
        oss << "[" << label << "] running=" << is_running()
            << " exit_code=" << (code.has_value() ? std::to_string(*code) : "N/A")
            << " pid=" << pi.dwProcessId << "\n";
        oss << "--- output tail ---\n" << read_output_tail();
        return oss.str();
    }

    void remove_log() const
    {
        if (!log_path.empty())
            std::filesystem::remove(log_path);
    }

    void terminate()
    {
        if (pi.hProcess == nullptr || !is_running())
            return;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }

private:
    void close_handles()
    {
        if (pi.hThread != nullptr)
        {
            ::CloseHandle(pi.hThread);
            pi.hThread = nullptr;
        }
        if (pi.hProcess != nullptr)
        {
            ::CloseHandle(pi.hProcess);
            pi.hProcess = nullptr;
        }
    }
};
#endif

auto write_entity_defs_json() -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "atlas_login_flow_entity_defs.json";
    std::ofstream f(path, std::ios::trunc);
    f << R"({
        "version": 1,
        "types": [
            {
                "type_id": 1,
                "name": "Account",
                "has_cell": false,
                "has_client": true,
                "properties": [
                    {"name": "accountName", "type": "string", "persistent": true,
                     "identifier": true, "scope": "base_only", "index": 0},
                    {"name": "level", "type": "int32", "persistent": true,
                     "identifier": false, "scope": "base_only", "index": 1}
                ]
            }
        ]
    })";
    return path;
}

auto write_baseapp_config_json(const std::filesystem::path& runtime_cfg,
                               const std::filesystem::path& runtime_dll) -> std::filesystem::path
{
    auto path = std::filesystem::temp_directory_path() / "atlas_login_flow_baseapp_config.json";
    std::ofstream f(path, std::ios::trunc);
    f << std::format(
        R"({{
    "script": {{
        "assembly": "{}",
        "runtime_config": "{}"
    }}
}})",
        runtime_dll.generic_string(), runtime_cfg.generic_string());
    return path;
}

struct ClientHarness
{
    explicit ClientHarness(std::string name) : dispatcher(std::move(name)), network(dispatcher)
    {
        dispatcher.set_max_poll_wait(Milliseconds(1));
        (void)network.interface_table().register_typed_handler<login::LoginResult>(
            [this](const Address&, Channel*, const login::LoginResult& msg)
            {
                login_result = msg;
                login_received.store(true, std::memory_order_release);
            });
        (void)network.interface_table().register_typed_handler<baseapp::AuthenticateResult>(
            [this](const Address&, Channel*, const baseapp::AuthenticateResult& msg)
            {
                auth_result = msg;
                auth_received.store(true, std::memory_order_release);
            });
        (void)network.interface_table().register_typed_handler<login::AllocateBaseAppResult>(
            [this](const Address&, Channel*, const login::AllocateBaseAppResult& msg)
            {
                allocate_result = msg;
                allocate_received.store(true, std::memory_order_release);
            });
    }

    void start_local_rudp(uint16_t port)
    {
        ASSERT_TRUE(network.start_rudp_server(Address("127.0.0.1", port)).has_value());
    }

    void reset_login() { login_received.store(false, std::memory_order_release); }
    void reset_auth() { auth_received.store(false, std::memory_order_release); }
    void reset_allocate() { allocate_received.store(false, std::memory_order_release); }

    EventDispatcher dispatcher;
    NetworkInterface network;
    std::atomic<bool> login_received{false};
    std::atomic<bool> auth_received{false};
    std::atomic<bool> allocate_received{false};
    login::LoginResult login_result;
    baseapp::AuthenticateResult auth_result;
    login::AllocateBaseAppResult allocate_result;
};

auto wait_for_process_registration(
    MachinedClient& machined_client, EventDispatcher& dispatcher, ProcessType type,
    uint16_t expected_internal_port, std::optional<uint16_t> expected_external_port = std::nullopt,
    std::vector<machined::ProcessInfo>* last_entries = nullptr,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) -> bool
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto entries = machined_client.query_sync(type, std::chrono::milliseconds(300));
        if (last_entries != nullptr)
            *last_entries = entries;
        for (const auto& entry : entries)
        {
            if (entry.internal_addr.port() != expected_internal_port)
                continue;
            if (expected_external_port.has_value() &&
                entry.external_addr.port() != *expected_external_port)
            {
                continue;
            }

            return true;
        }
        dispatcher.process_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

auto format_process_entries(const std::vector<machined::ProcessInfo>& entries) -> std::string
{
    if (entries.empty())
        return "(none)";

    std::ostringstream oss;
    bool first = true;
    for (const auto& entry : entries)
    {
        if (!first)
            oss << "; ";
        first = false;
        oss << std::format("name={} internal={} external={} pid={} load={:.3f}", entry.name,
                           entry.internal_addr.to_string(), entry.external_addr.to_string(),
                           entry.pid, entry.load);
    }
    return oss.str();
}

}  // namespace

TEST(LoginFlowIntegration, ClientCanLoginAndAuthenticateThroughFullStack)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows-only process harness";
#else
    const auto runtime_cfg = runtime_config_path();
    const auto runtime_dll = runtime_assembly_path();
    if (!std::filesystem::exists(runtime_cfg) || !std::filesystem::exists(runtime_dll))
    {
        GTEST_SKIP() << "runtime prerequisites missing: " << runtime_cfg << " / " << runtime_dll;
    }

    const auto machined_exe = server_bin_dir() / "machined.exe";
    const auto dbapp_exe = server_bin_dir() / "atlas_dbapp.exe";
    const auto baseappmgr_exe = server_bin_dir() / "atlas_baseappmgr.exe";
    const auto baseapp_exe = server_bin_dir() / "atlas_baseapp.exe";
    const auto loginapp_exe = server_bin_dir() / "atlas_loginapp.exe";
    if (!std::filesystem::exists(machined_exe) || !std::filesystem::exists(dbapp_exe) ||
        !std::filesystem::exists(baseappmgr_exe) || !std::filesystem::exists(baseapp_exe) ||
        !std::filesystem::exists(loginapp_exe))
    {
        GTEST_SKIP() << "server binaries missing under " << server_bin_dir();
    }

    const auto entity_defs = write_entity_defs_json();
    const auto baseapp_config = write_baseapp_config_json(runtime_cfg, runtime_dll);
    const auto sqlite_path = std::filesystem::temp_directory_path() / "atlas_login_flow.sqlite3";
    std::filesystem::remove(sqlite_path);

    const uint16_t machined_port = reserve_tcp_port();
    const uint16_t dbapp_port = reserve_udp_port();
    const uint16_t baseappmgr_port = reserve_udp_port();
    const uint16_t baseapp_internal_port = reserve_udp_port();
    const uint16_t baseapp_external_port = reserve_udp_port();
    const uint16_t loginapp_internal_port = reserve_udp_port();
    const uint16_t loginapp_external_port = reserve_udp_port();

    ASSERT_NE(machined_port, 0u);
    ASSERT_NE(dbapp_port, 0u);
    ASSERT_NE(baseappmgr_port, 0u);
    ASSERT_NE(baseapp_internal_port, 0u);
    ASSERT_NE(baseapp_external_port, 0u);
    ASSERT_NE(loginapp_internal_port, 0u);
    ASSERT_NE(loginapp_external_port, 0u);

    const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);

    ChildProcess machined = ChildProcess::launch(
        machined_exe,
        {L"--type", L"machined", L"--name", L"machined_login_flow_test", L"--update-hertz", L"100",
         L"--internal-port", std::to_wstring(machined_port)},
        server_bin_dir(), "machined");
    ASSERT_TRUE(machined.is_running()) << machined.diagnostic();

    EventDispatcher machined_dispatcher{"login_flow_machined_bootstrap"};
    machined_dispatcher.set_max_poll_wait(Milliseconds(1));
    NetworkInterface machined_network(machined_dispatcher);
    MachinedClient machined_client(machined_dispatcher, machined_network);
    ASSERT_TRUE(machined_client.connect(Address("127.0.0.1", machined_port)));

    ChildProcess dbapp = ChildProcess::launch(dbapp_exe,
                                              {L"--type",
                                               L"dbapp",
                                               L"--name",
                                               L"dbapp_login_flow_test",
                                               L"--update-hertz",
                                               L"100",
                                               L"--internal-port",
                                               std::to_wstring(dbapp_port),
                                               L"--machined",
                                               machined_addr,
                                               L"--entitydef-path",
                                               entity_defs.wstring(),
                                               L"--db-type",
                                               L"sqlite",
                                               L"--db-sqlite-path",
                                               sqlite_path.wstring(),
                                               L"--account-type-id",
                                               L"1",
                                               L"--auto-create-accounts",
                                               L"true"},
                                              server_bin_dir(), "dbapp");
    ASSERT_TRUE(dbapp.is_running()) << dbapp.diagnostic();

    ChildProcess baseappmgr =
        ChildProcess::launch(baseappmgr_exe,
                             {L"--type", L"baseappmgr", L"--name", L"baseappmgr_login_flow_test",
                              L"--update-hertz", L"100", L"--internal-port",
                              std::to_wstring(baseappmgr_port), L"--machined", machined_addr},
                             server_bin_dir(), "baseappmgr");
    ASSERT_TRUE(baseappmgr.is_running()) << baseappmgr.diagnostic();

    ChildProcess baseapp = ChildProcess::launch(
        baseapp_exe,
        {L"--type", L"baseapp", L"--name", L"baseapp_login_flow_test", L"--config",
         baseapp_config.wstring(), L"--update-hertz", L"100", L"--internal-port",
         std::to_wstring(baseapp_internal_port), L"--external-port",
         std::to_wstring(baseapp_external_port), L"--machined", machined_addr},
        server_bin_dir(), "baseapp");
    ASSERT_TRUE(baseapp.is_running()) << baseapp.diagnostic();

    ChildProcess loginapp = ChildProcess::launch(
        loginapp_exe,
        {L"--type", L"loginapp", L"--name", L"loginapp_login_flow_test", L"--update-hertz", L"100",
         L"--internal-port", std::to_wstring(loginapp_internal_port), L"--external-port",
         std::to_wstring(loginapp_external_port), L"--machined", machined_addr,
         L"--auto-create-accounts", L"true"},
        server_bin_dir(), "loginapp");
    ASSERT_TRUE(loginapp.is_running()) << loginapp.diagnostic();

    EventDispatcher registry_dispatcher{"login_flow_registry_client"};
    registry_dispatcher.set_max_poll_wait(Milliseconds(1));
    NetworkInterface registry_network(registry_dispatcher);
    MachinedClient registry_client(registry_dispatcher, registry_network);
    ASSERT_TRUE(registry_client.connect(Address("127.0.0.1", machined_port)));

    std::vector<machined::ProcessInfo> dbapp_entries;
    ASSERT_TRUE(wait_for_process_registration(registry_client, registry_dispatcher,
                                              ProcessType::DBApp, dbapp_port, std::nullopt,
                                              &dbapp_entries))
        << "DBApp did not register with machined; entries=" << format_process_entries(dbapp_entries)
        << "\n"
        << dbapp.diagnostic();

    std::vector<machined::ProcessInfo> baseappmgr_entries;
    ASSERT_TRUE(wait_for_process_registration(registry_client, registry_dispatcher,
                                              ProcessType::BaseAppMgr, baseappmgr_port,
                                              std::nullopt, &baseappmgr_entries))
        << "BaseAppMgr did not register with machined; entries="
        << format_process_entries(baseappmgr_entries) << "\n"
        << baseappmgr.diagnostic();

    std::vector<machined::ProcessInfo> baseapp_entries;
    ASSERT_TRUE(wait_for_process_registration(registry_client, registry_dispatcher,
                                              ProcessType::BaseApp, baseapp_internal_port,
                                              baseapp_external_port, &baseapp_entries))
        << "BaseApp did not register with machined; entries="
        << format_process_entries(baseapp_entries) << "\n"
        << baseapp.diagnostic();

    std::vector<machined::ProcessInfo> loginapp_entries;
    ASSERT_TRUE(wait_for_process_registration(registry_client, registry_dispatcher,
                                              ProcessType::LoginApp, loginapp_internal_port,
                                              loginapp_external_port, &loginapp_entries))
        << "LoginApp did not register with machined; entries="
        << format_process_entries(loginapp_entries) << "\n"
        << loginapp.diagnostic();

    ClientHarness client{"login_flow_rudp_client"};
    client.start_local_rudp(reserve_udp_port());

    auto bam_channel = client.network.connect_rudp(Address("127.0.0.1", baseappmgr_port));
    ASSERT_TRUE(bam_channel.has_value()) << bam_channel.error().message();

    bool allocated = false;
    const auto allocate_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < allocate_deadline)
    {
        ASSERT_TRUE(baseappmgr.is_running());
        client.reset_allocate();
        login::AllocateBaseApp alloc;
        alloc.request_id = 9001;
        alloc.type_id = 1;
        alloc.dbid = 42;
        ASSERT_TRUE((*bam_channel)->send_message(alloc).has_value());
        if (poll_until(
                client.dispatcher,
                [&] { return client.allocate_received.load(std::memory_order_acquire); },
                std::chrono::milliseconds(400)) &&
            client.allocate_result.success)
        {
            allocated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(allocated) << "BaseAppMgr never observed a ready BaseApp\n"
                           << baseappmgr.diagnostic() << baseapp.diagnostic();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto login_channel = client.network.connect_rudp(Address("127.0.0.1", loginapp_external_port));
    ASSERT_TRUE(login_channel.has_value()) << login_channel.error().message();

    login::LoginRequest login_req;
    login_req.username = "hero";
    login_req.password_hash = "pw_hash";
    ASSERT_TRUE((*login_channel)->send_message(login_req).has_value());

    ASSERT_TRUE(poll_until(
        client.dispatcher, [&] { return client.login_received.load(std::memory_order_acquire); },
        std::chrono::seconds(10)))
        << "LoginResult not received\n"
        << loginapp.diagnostic() << dbapp.diagnostic();
    ASSERT_TRUE(loginapp.is_running()) << loginapp.diagnostic();
    EXPECT_EQ(client.login_result.status, login::LoginStatus::Success);
    EXPECT_TRUE(client.login_result.error_message.empty());
    EXPECT_EQ(client.login_result.baseapp_addr.port(), baseapp_external_port);

    client.reset_auth();
    auto baseapp_channel = client.network.connect_rudp(client.login_result.baseapp_addr);
    ASSERT_TRUE(baseapp_channel.has_value()) << baseapp_channel.error().message();

    baseapp::Authenticate auth;
    auth.session_key = client.login_result.session_key;
    ASSERT_TRUE((*baseapp_channel)->send_message(auth).has_value());

    ASSERT_TRUE(poll_until(
        client.dispatcher, [&] { return client.auth_received.load(std::memory_order_acquire); },
        std::chrono::seconds(10)))
        << "AuthenticateResult not received\n"
        << baseapp.diagnostic();
    ASSERT_TRUE(baseapp.is_running()) << baseapp.diagnostic();
    EXPECT_TRUE(client.auth_result.success);
    EXPECT_GT(client.auth_result.entity_id, 0u);
    EXPECT_EQ(client.auth_result.type_id, 1u);
    EXPECT_TRUE(client.auth_result.error.empty());

    std::filesystem::remove(sqlite_path);
    std::filesystem::remove(entity_defs);
    std::filesystem::remove(baseapp_config);
    machined.remove_log();
    dbapp.remove_log();
    baseappmgr.remove_log();
    baseapp.remove_log();
    loginapp.remove_log();
#endif
}
