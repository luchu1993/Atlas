#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "cellappmgr/cellappmgr_messages.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"
#include "network/tcp_channel.h"
#include "server/machined_client.h"

#if defined(_WIN32)
#include <windows.h>
#undef SendMessage  // collides with Channel::SendMessage
#endif

using namespace atlas;
using namespace atlas::cellappmgr;

namespace {

#if defined(_WIN32)

template <typename Pred>
bool PollUntil(EventDispatcher& disp, Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

template <typename Pred>
bool PollUntil(EventDispatcher& first, EventDispatcher& second, Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    first.ProcessOnce();
    second.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

auto ReserveUdpPort() -> uint16_t {
  auto sock = Socket::CreateUdp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  return local ? local->Port() : 0;
}

auto ReserveTcpPort() -> uint16_t {
  auto sock = Socket::CreateTcp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  return local ? local->Port() : 0;
}

auto ExecutablePath() -> std::filesystem::path {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(len);
  return std::filesystem::path(buffer);
}

auto BuildRoot() -> std::filesystem::path {
  // The machined target output is a stable marker for the CMake build root.
  auto current = ExecutablePath().parent_path();
  for (int i = 0; i < 10 && !current.empty(); ++i) {
    if (std::filesystem::exists(current / "src" / "server" / "machined" / "Debug" /
                                "machined.exe")) {
      return current;
    }
    current = current.parent_path();
  }
  return {};
}

auto ServerBinDir() -> std::filesystem::path {
  return BuildRoot() / "bin" / "Debug";
}

auto ResolveServerExe(const std::wstring& subdir, const std::wstring& filename)
    -> std::filesystem::path {
  auto p1 = ServerBinDir() / filename;
  if (std::filesystem::exists(p1)) return std::filesystem::absolute(p1);
  auto p2 = BuildRoot() / "src" / "server" / subdir / "Debug" / filename;
  if (std::filesystem::exists(p2)) return std::filesystem::absolute(p2);
  return {};
}

auto QuoteArg(const std::wstring& arg) -> std::wstring {
  std::wstring quoted = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'"') quoted += L'\\';
    quoted += ch;
  }
  quoted += L"\"";
  return quoted;
}

struct Child {
  PROCESS_INFORMATION pi{};
  std::string label;
  std::filesystem::path log_path;

  static auto Launch(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                     const std::string& proc_label) -> Child {
    std::wstring cmd = QuoteArg(exe.wstring());
    for (const auto& a : args) {
      cmd += L' ';
      cmd += QuoteArg(a);
    }
    // Include PID and tick count so sibling test cases never overwrite diagnostics.
    const auto log_stamp =
        std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
    auto log_file = std::filesystem::temp_directory_path() /
                    ("atlas_cellappmgr_process_" + proc_label + "_" + log_stamp + ".log");
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE h_log = ::CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (h_log != INVALID_HANDLE_VALUE) {
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = h_log;
      si.hStdError = h_log;
      si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const auto workdir = exe.parent_path();
    const BOOL ok = ::CreateProcessW(exe.wstring().c_str(), buf.data(), nullptr, nullptr,
                                     h_log != INVALID_HANDLE_VALUE ? TRUE : FALSE, 0, nullptr,
                                     workdir.wstring().c_str(), &si, &pi);
    EXPECT_TRUE(ok) << "CreateProcessW failed for " << exe.string();
    if (h_log != INVALID_HANDLE_VALUE) ::CloseHandle(h_log);
    Child c;
    c.pi = pi;
    c.label = proc_label;
    c.log_path = log_file;
    return c;
  }

  [[nodiscard]] auto IsRunning() const -> bool {
    if (pi.hProcess == nullptr) return false;
    DWORD code = 0;
    return ::GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE;
  }

  [[nodiscard]] auto Diagnostic() const -> std::string {
    std::string out = "[" + label + "] running=" + (IsRunning() ? "yes" : "no");
    if (pi.hProcess != nullptr) {
      DWORD code = 0;
      if (::GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
        out += " exit=" + std::to_string(code);
      }
    }
    // Tail logs into assertion output so subprocess startup failures stay visible.
    if (!log_path.empty() && std::filesystem::exists(log_path)) {
      std::ifstream f(log_path, std::ios::in);
      std::deque<std::string> ring;
      std::string line;
      while (std::getline(f, line)) {
        ring.push_back(std::move(line));
        if (ring.size() > 20) ring.pop_front();
      }
      out += "\n--- log tail ---\n";
      for (const auto& l : ring) out += "  " + l + "\n";
    }
    return out;
  }

  ~Child() {
    if (pi.hProcess != nullptr) {
      if (IsRunning()) ::TerminateProcess(pi.hProcess, 1);
      ::WaitForSingleObject(pi.hProcess, 5000);
      ::CloseHandle(pi.hProcess);
      pi.hProcess = nullptr;
    }
    if (pi.hThread != nullptr) {
      ::CloseHandle(pi.hThread);
      pi.hThread = nullptr;
    }
  }

  Child() = default;
  Child(const Child&) = delete;
  auto operator=(const Child&) -> Child& = delete;
  Child(Child&& o) noexcept : pi(o.pi), label(std::move(o.label)), log_path(std::move(o.log_path)) {
    o.pi = {};
  }
  auto operator=(Child&& o) noexcept -> Child& {
    if (this == &o) return *this;
    this->~Child();
    pi = o.pi;
    label = std::move(o.label);
    log_path = std::move(o.log_path);
    o.pi = {};
    return *this;
  }
};

auto WaitForRegistration(MachinedClient& client, EventDispatcher& disp, ProcessType type,
                         uint16_t advertised_port) -> bool {
  return PollUntil(disp, [&]() {
    auto infos = client.QuerySync(type, std::chrono::milliseconds(200));
    for (const auto& p : infos) {
      if (p.internal_addr.Port() == advertised_port) return true;
    }
    return false;
  });
}

auto WaitForNamedRegistration(MachinedClient& client, EventDispatcher& disp, ProcessType type,
                              const std::string& name,
                              machined::ProcessInfo* out = nullptr) -> bool {
  return PollUntil(disp, [&]() {
    auto infos = client.QuerySync(type, std::chrono::milliseconds(200));
    for (const auto& p : infos) {
      if (p.name != name) continue;
      if (out != nullptr) *out = p;
      return true;
    }
    return false;
  });
}

auto WaitForNamedRegistrationWithDifferentPid(MachinedClient& client, EventDispatcher& disp,
                                              ProcessType type, const std::string& name,
                                              uint32_t previous_pid,
                                              machined::ProcessInfo* out) -> bool {
  return PollUntil(disp, [&]() {
    auto infos = client.QuerySync(type, std::chrono::milliseconds(200));
    for (const auto& p : infos) {
      if (p.name != name || p.pid == previous_pid) continue;
      *out = p;
      return true;
    }
    return false;
  });
}

auto TerminatePid(uint32_t pid) -> bool {
  HANDLE proc = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (proc == nullptr) return false;
  const BOOL ok = ::TerminateProcess(proc, 1);
  ::CloseHandle(proc);
  return ok != FALSE;
}

struct PidGuard {
  uint32_t pid{0};

  ~PidGuard() {
    if (pid != 0) (void)TerminatePid(pid);
  }

  void Reset(uint32_t next_pid) {
    if (pid != 0 && pid != next_pid) (void)TerminatePid(pid);
    pid = next_pid;
  }

  void Forget() { pid = 0; }
};

auto TcpConnectProbe(uint16_t port) -> bool {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(0x7F000001u);  // 127.0.0.1
  // Keep each connect attempt short; PollUntil repeats the probe on loopback.
  DWORD send_timeout = 250;
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout),
               sizeof(send_timeout));
  const int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
  ::closesocket(s);
  return rc == 0;
}

auto WaitForTcpListen(uint16_t port, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (TcpConnectProbe(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

auto WaitForUdpBound(uint16_t /*port*/, std::chrono::milliseconds timeout) -> bool {
  // Atlas UDP sockets use SO_REUSEADDR, so a fixed wait is safer than bind probing.
  std::this_thread::sleep_for((std::min)(timeout, std::chrono::milliseconds(250)));
  return true;
}

constexpr int kLaunchRetryCount = 5;

auto LaunchMachinedWithRetry(const std::filesystem::path& machined_exe,
                             const std::wstring& name_suffix, uint16_t* out_port, Child* out_child)
    -> bool {
  for (int attempt = 0; attempt < kLaunchRetryCount; ++attempt) {
    const uint16_t port = ReserveTcpPort();
    if (port == 0) continue;
    auto child =
        Child::Launch(machined_exe,
                      {L"--type", L"machined", L"--name", L"machined_" + name_suffix,
                       L"--update-hertz", L"100", L"--internal-port", std::to_wstring(port)},
                      "machined");
    if (child.IsRunning() && WaitForTcpListen(port, std::chrono::seconds(2))) {
      *out_port = port;
      *out_child = std::move(child);
      return true;
    }
    // Destructor terminates and cleans up the child if the probe failed.
  }
  return false;
}

auto LaunchCellAppMgrWithRetry(const std::filesystem::path& cellappmgr_exe,
                               const std::wstring& machined_addr, const std::wstring& name_suffix,
                               uint16_t* out_port, Child* out_child) -> bool {
  for (int attempt = 0; attempt < kLaunchRetryCount; ++attempt) {
    const uint16_t port = ReserveUdpPort();
    if (port == 0) continue;
    auto child = Child::Launch(
        cellappmgr_exe,
        {L"--type", L"cellappmgr", L"--name", L"cellappmgr_" + name_suffix, L"--update-hertz",
         L"100", L"--internal-port", std::to_wstring(port), L"--machined", machined_addr},
        "cellappmgr");
    if (child.IsRunning() && WaitForUdpBound(port, std::chrono::seconds(2))) {
      *out_port = port;
      *out_child = std::move(child);
      return true;
    }
  }
  return false;
}

auto LaunchReviver(const std::filesystem::path& reviver_exe,
                   const std::filesystem::path& cellappmgr_exe,
                   const std::wstring& machined_addr, uint16_t cellappmgr_port,
                   const std::filesystem::path& snapshot_path,
                   const std::filesystem::path& leader_lock_path,
                   const std::wstring& reviver_name = L"reviver_process_test",
                   uint32_t max_restarts = 3,
                   uint32_t health_failure_threshold = 2,
                   uint32_t manager_health_timeout_ms = 5000,
                   uint32_t launch_timeout_ms = 5000,
                   uint32_t snapshot_interval_ms = 250,
                   const std::filesystem::path& config_path = {},
                   const std::filesystem::path& output_path = {},
                   uint32_t heartbeat_timeout_ms = 500) -> Child {
  std::vector<std::wstring> args{
      L"--type", L"reviver", L"--name", reviver_name, L"--update-hertz", L"100",
      L"--machined", machined_addr, L"--revive-cellappmgr-exe", cellappmgr_exe.wstring(),
      L"--revive-cellappmgr-name", L"cellappmgr_revived", L"--revive-cellappmgr-port",
      std::to_wstring(cellappmgr_port), L"--revive-cellappmgr-on-start", L"true",
      L"--revive-cellappmgr-snapshot-path", snapshot_path.wstring(),
      L"--revive-cellappmgr-snapshot-interval-ms", std::to_wstring(snapshot_interval_ms),
      L"--revive-leader-lock-path", leader_lock_path.wstring(),
      L"--revive-cellappmgr-update-hertz", L"100",
      L"--revive-cellappmgr-launch-timeout-ms", std::to_wstring(launch_timeout_ms),
      L"--revive-cellappmgr-health-interval-ms", L"50",
      L"--revive-cellappmgr-heartbeat-timeout-ms",
      std::to_wstring(heartbeat_timeout_ms),
      L"--revive-cellappmgr-manager-health-timeout-ms",
      std::to_wstring(manager_health_timeout_ms),
      L"--revive-cellappmgr-health-failure-threshold",
      std::to_wstring(health_failure_threshold),
      L"--revive-cellappmgr-audit-interval-ms", L"50",
      L"--revive-cellappmgr-missing-audit-threshold", L"2", L"--revive-restart-delay-ms", L"50",
      L"--revive-max-restarts", std::to_wstring(max_restarts)};
  if (!config_path.empty()) {
    args.push_back(L"--config");
    args.push_back(config_path.wstring());
  }
  if (!output_path.empty()) {
    args.push_back(L"--revive-cellappmgr-output-path");
    args.push_back(output_path.wstring());
  }
  return Child::Launch(reviver_exe, args, "reviver");
}

auto FileContains(const std::filesystem::path& path, std::string_view needle) -> bool {
  std::ifstream file(path, std::ios::in);
  std::string line;
  while (std::getline(file, line)) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

auto QueryWatcherValue(MachinedClient& client, EventDispatcher& disp, ProcessType type,
                       const std::string& name, const std::string& path)
    -> std::optional<std::string> {
  bool done = false;
  bool found = false;
  std::string value;
  client.QueryWatcher(type, name, path,
                      [&](bool result_found, const std::string&, const std::string& result_value) {
                        found = result_found;
                        value = result_value;
                        done = true;
                      });
  if (!PollUntil(disp, [&] { return done; }, std::chrono::milliseconds(1000))) {
    return std::nullopt;
  }
  if (!found) return std::nullopt;
  return value;
}

#endif  // defined(_WIN32)

}  // namespace

TEST(MachinedClient, AsyncQueryTimesOutOnSilentConnection) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  EventDispatcher server_disp{"silent_machined_query_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_net(server_disp);
  auto listen = server_net.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(listen.HasValue()) << listen.Error().Message();

  EventDispatcher client_disp{"silent_machined_query_client"};
  client_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface client_net(client_disp);
  MachinedClient client(client_disp, client_net, Milliseconds(100));
  ASSERT_TRUE(client.Connect(server_net.TcpAddress()));

  bool called = false;
  std::vector<machined::ProcessInfo> result{{}};
  client.QueryAsync(ProcessType::kCellAppMgr, [&](std::vector<machined::ProcessInfo> infos) {
    result = std::move(infos);
    called = true;
  });

  ASSERT_TRUE(PollUntil(client_disp, server_disp, [&] {
    client.Tick();
    client_net.FlushDirtySendChannels();
    server_net.FlushDirtySendChannels();
    return called;
  }, std::chrono::milliseconds(1500)));
  EXPECT_TRUE(result.empty());
#endif
}

TEST(MachinedClient, WatcherQueryTimesOutOnSilentConnection) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  EventDispatcher server_disp{"silent_machined_watcher_server"};
  server_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server_net(server_disp);
  auto listen = server_net.StartTcpServer(Address("127.0.0.1", 0));
  ASSERT_TRUE(listen.HasValue()) << listen.Error().Message();

  EventDispatcher client_disp{"silent_machined_watcher_client"};
  client_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface client_net(client_disp);
  MachinedClient client(client_disp, client_net, Milliseconds(100));
  ASSERT_TRUE(client.Connect(server_net.TcpAddress()));

  bool called = false;
  bool found = true;
  std::string source_name;
  std::string value{"unchanged"};
  const auto rid = client.QueryWatcher(
      ProcessType::kCellAppMgr, "cellappmgr_missing", "app/uptime_seconds",
      [&](bool result_found, const std::string& result_source, const std::string& result_value) {
        found = result_found;
        source_name = result_source;
        value = result_value;
        called = true;
      });
  ASSERT_NE(rid, 0u);

  ASSERT_TRUE(PollUntil(client_disp, server_disp, [&] {
    client.Tick();
    client_net.FlushDirtySendChannels();
    server_net.FlushDirtySendChannels();
    return called;
  }, std::chrono::milliseconds(1500)));
  EXPECT_FALSE(found);
  EXPECT_EQ(source_name, "cellappmgr_missing");
  EXPECT_TRUE(value.empty());
#endif
}

TEST(CellAppMgrProcess, MachinedAndCellAppMgrBootAndRegister) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"cellappmgr_boot", &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  uint16_t cellappmgr_port = 0;
  Child cellappmgr;
  ASSERT_TRUE(LaunchCellAppMgrWithRetry(cellappmgr_exe, machined_addr, L"process_test",
                                        &cellappmgr_port, &cellappmgr))
      << "cellappmgr failed to start + bind UDP on any attempt\n"
      << machined.Diagnostic();

  EventDispatcher disp{"cellappmgr_process_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  ASSERT_TRUE(WaitForRegistration(client, disp, ProcessType::kCellAppMgr, cellappmgr_port))
      << "atlas_cellappmgr.exe did not register with machined - " << machined.Diagnostic() << "\n"
      << cellappmgr.Diagnostic();

  bool set_done = false;
  bool set_found = false;
  std::string set_value;
  client.SetWatcher(ProcessType::kCellAppMgr, "", "cellappmgr/lb/retire/app_id", "0",
                    [&](bool found, const std::string&, const std::string& value) {
                      set_found = found;
                      set_value = value;
                      set_done = true;
                    });
  ASSERT_TRUE(PollUntil(disp, [&] { return set_done; }))
      << "set-watch response not received from real cellappmgr binary - "
      << machined.Diagnostic() << "\n"
      << cellappmgr.Diagnostic();
  EXPECT_TRUE(set_found);
  EXPECT_EQ(set_value, "0");

  bool query_done = false;
  bool query_found = false;
  std::string query_value;
  client.QueryWatcher(ProcessType::kCellAppMgr, "", "cellappmgr/lb/retire/app_id",
                      [&](bool found, const std::string&, const std::string& value) {
                        query_found = found;
                        query_value = value;
                        query_done = true;
                      });
  ASSERT_TRUE(PollUntil(disp, [&] { return query_done; }))
      << "watch response not received from real cellappmgr binary - "
      << machined.Diagnostic() << "\n"
      << cellappmgr.Diagnostic();
  EXPECT_TRUE(query_found);
  EXPECT_EQ(query_value, "0");
#endif
}

TEST(CellAppMgrProcess, SyntheticCellAppRegistersWithRealCellAppMgrBinary) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"cellapp_register", &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  uint16_t cellappmgr_port = 0;
  Child cellappmgr;
  ASSERT_TRUE(LaunchCellAppMgrWithRetry(cellappmgr_exe, machined_addr, L"register_test",
                                        &cellappmgr_port, &cellappmgr))
      << "cellappmgr failed to start + bind UDP on any attempt\n"
      << machined.Diagnostic();

  EventDispatcher registry_disp{"registry_probe"};
  registry_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface registry_net(registry_disp);
  MachinedClient registry_client(registry_disp, registry_net);
  ASSERT_TRUE(registry_client.Connect(Address("127.0.0.1", machined_port)));
  ASSERT_TRUE(WaitForRegistration(registry_client, registry_disp, ProcessType::kCellAppMgr,
                                  cellappmgr_port))
      << machined.Diagnostic() << "\n"
      << cellappmgr.Diagnostic();

  // The synthetic register flow verifies standalone CellAppMgr RUDP handler wiring.
  EventDispatcher disp{"synthetic_cellapp"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  std::atomic<bool> ack_received{false};
  RegisterCellAppAck ack_copy;
  net.InterfaceTable().RegisterTypedHandler<RegisterCellAppAck>(
      [&](const Address&, Channel*, const RegisterCellAppAck& msg) {
        ack_copy = msg;
        ack_received.store(true, std::memory_order_release);
      });

  auto ch = net.ConnectRudp(Address("127.0.0.1", cellappmgr_port));
  ASSERT_TRUE(ch.HasValue()) << ch.Error().Message();

  RegisterCellApp reg;
  reg.internal_addr = Address(0, 32001);
  ASSERT_TRUE((*ch)->SendMessage(reg).HasValue());

  ASSERT_TRUE(PollUntil(disp, [&] { return ack_received.load(std::memory_order_acquire); }))
      << "RegisterCellAppAck not received from real cellappmgr binary - "
      << cellappmgr.Diagnostic();
  EXPECT_TRUE(ack_copy.success);
  EXPECT_EQ(ack_copy.app_id, 1u);
#endif
}

TEST(CellAppMgrProcess, ReviverColdStartsAndRestartsCellAppMgr) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver", &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  const auto snapshot_stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_cellappmgr_snapshot_" + snapshot_stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_cellappmgr_lock_" + snapshot_stamp + ".lock");
  const auto reviver_config_path = std::filesystem::temp_directory_path() /
                                   ("atlas_reviver_cellappmgr_config_" + snapshot_stamp + ".json");
  const auto revived_output_path = std::filesystem::temp_directory_path() /
                                   ("atlas_reviver_cellappmgr_output_" + snapshot_stamp + ".log");
  {
    std::ofstream config(reviver_config_path, std::ios::out | std::ios::trunc);
    config << R"({"cellappmgr_lb_tick_load_weight":2.5})";
  }
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
  std::filesystem::remove(revived_output_path, ec);

  PidGuard revived_mgr;
  Child reviver = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_process_test", 3, 2, 5000, 5000, 250,
                                reviver_config_path, revived_output_path);
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();

  EventDispatcher disp{"reviver_process_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver,
                                       "reviver_process_test"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  machined::ProcessInfo first_mgr;
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kCellAppMgr,
                                       "cellappmgr_revived", &first_mgr))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();
  EXPECT_EQ(first_mgr.internal_addr.Port(), cellappmgr_port);
  ASSERT_NE(first_mgr.pid, 0u);
  revived_mgr.Reset(first_mgr.pid);

  const auto snapshot_interval = QueryWatcherValue(client, disp, ProcessType::kCellAppMgr,
                                                   "cellappmgr_revived",
                                                   "cellappmgr/ha/snapshot_interval_ms");
  ASSERT_TRUE(snapshot_interval.has_value());
  EXPECT_EQ(*snapshot_interval, "250");
  const auto tick_load_weight = QueryWatcherValue(client, disp, ProcessType::kCellAppMgr,
                                                  "cellappmgr_revived",
                                                  "cellappmgr/lb/weights/tick_load");
  ASSERT_TRUE(tick_load_weight.has_value());
  EXPECT_EQ(*tick_load_weight, "2.500000");
  // Regression guard for the multi-target watcher path: this watcher key
  // is built as std::format("reviver/{}", t.slug) + "/output_path"
  // inside RegisterTargetWatchers. If a future refactor delays
  // ManagedTarget::slug initialisation past ServerApp::Init's
  // RegisterWatchers() call, the key bakes to "reviver//output_path"
  // (empty slug segment) and this has_value() assertion fails. Keep
  // this ASSERT load-bearing; deleting it removes the only test that
  // catches Reviver::Init ordering regressions of the slug field.
  const auto output_path = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                             "reviver_process_test",
                                             "reviver/cellappmgr/output_path");
  ASSERT_TRUE(output_path.has_value());
  EXPECT_EQ(std::filesystem::path(*output_path), revived_output_path);
  ASSERT_TRUE(PollUntil(disp, [&] {
    return FileContains(revived_output_path, "cellappmgr_revived started");
  })) << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto audits = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_process_test",
                                          "reviver/cellappmgr/registry_audits");
    if (!audits) return false;
    return std::stoi(*audits) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto missing = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                         "reviver_process_test",
                                         "reviver/cellappmgr/registry_missing");
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ(*missing, "0");
  const auto liveness = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_process_test",
                                          "reviver/cellappmgr/liveness_failures");
  ASSERT_TRUE(liveness.has_value());
  EXPECT_EQ(*liveness, "0");
  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto checks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_process_test",
                                          "reviver/cellappmgr/health_checks");
    if (!checks) return false;
    return std::stoi(*checks) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto health_failures = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                  "reviver_process_test",
                                                  "reviver/cellappmgr/health_failures");
  ASSERT_TRUE(health_failures.has_value());
  EXPECT_EQ(*health_failures, "0");
  const auto manager_health_timeouts =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/manager_health_timeouts");
  ASSERT_TRUE(manager_health_timeouts.has_value());
  EXPECT_EQ(*manager_health_timeouts, "0");
  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_process_test",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto first_acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_process_test",
                                            "reviver/cellappmgr/heartbeat_acks");
  ASSERT_TRUE(first_acks.has_value());
  const auto first_ack_age = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                               "reviver_process_test",
                                               "reviver/cellappmgr/heartbeat_last_ack_age_ms");
  ASSERT_TRUE(first_ack_age.has_value());
  EXPECT_GE(std::stoll(*first_ack_age), 0);
  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto saves = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                         "reviver_process_test",
                                         "reviver/cellappmgr/heartbeat_snapshot_saves");
    if (!saves) return false;
    return std::stoull(*saves) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto heartbeat_snapshot_dirty =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/heartbeat_snapshot_dirty");
  ASSERT_TRUE(heartbeat_snapshot_dirty.has_value());
  EXPECT_EQ(*heartbeat_snapshot_dirty, "false");
  const auto heartbeat_snapshot_stale =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/heartbeat_snapshot_save_stale");
  ASSERT_TRUE(heartbeat_snapshot_stale.has_value());
  EXPECT_EQ(*heartbeat_snapshot_stale, "false");
  const auto heartbeat_snapshot_failures =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/heartbeat_snapshot_failures");
  ASSERT_TRUE(heartbeat_snapshot_failures.has_value());
  EXPECT_EQ(*heartbeat_snapshot_failures, "0");
  const auto heartbeat_snapshot_status =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/heartbeat_snapshot_status");
  ASSERT_TRUE(heartbeat_snapshot_status.has_value());
  EXPECT_NE(heartbeat_snapshot_status->find("state=ready"), std::string::npos);
  EXPECT_NE(heartbeat_snapshot_status->find("failures=0"), std::string::npos);
  EXPECT_NE(heartbeat_snapshot_status->find("dirty=0"), std::string::npos);
  EXPECT_NE(heartbeat_snapshot_status->find("stale=0"), std::string::npos);
  EXPECT_NE(heartbeat_snapshot_status->find("ack_age_ms="), std::string::npos);
  const auto first_restart_attempts =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/restart_attempts");
  ASSERT_TRUE(first_restart_attempts.has_value());
  EXPECT_EQ(*first_restart_attempts, "0");
  const auto heartbeat_sent = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                "reviver_process_test",
                                                "reviver/cellappmgr/heartbeat_sent");
  ASSERT_TRUE(heartbeat_sent.has_value());
  EXPECT_GE(std::stoi(*heartbeat_sent), 1);
  const auto heartbeat_timeout_ms = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                      "reviver_process_test",
                                                      "reviver/cellappmgr/heartbeat_timeout_ms");
  ASSERT_TRUE(heartbeat_timeout_ms.has_value());
  EXPECT_EQ(*heartbeat_timeout_ms, "500");
  const auto heartbeat_timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                    "reviver_process_test",
                                                    "reviver/cellappmgr/heartbeat_timeouts");
  ASSERT_TRUE(heartbeat_timeouts.has_value());
  EXPECT_EQ(*heartbeat_timeouts, "0");
  const auto forced_terminations =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/forced_terminations");
  ASSERT_TRUE(forced_terminations.has_value());
  EXPECT_EQ(*forced_terminations, "0");
  const auto status = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_process_test",
                                        "reviver/cellappmgr/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, "active");
  const auto launch_pending =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/launch_pending");
  ASSERT_TRUE(launch_pending.has_value());
  EXPECT_EQ(*launch_pending, "false");
  const auto first_generation =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/active_generation");
  ASSERT_TRUE(first_generation.has_value());
  EXPECT_EQ(*first_generation, "1");

  ASSERT_TRUE(TerminatePid(first_mgr.pid));
  revived_mgr.Forget();

  machined::ProcessInfo second_mgr;
  ASSERT_TRUE(WaitForNamedRegistrationWithDifferentPid(client, disp, ProcessType::kCellAppMgr,
                                                       "cellappmgr_revived", first_mgr.pid,
                                                       &second_mgr))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();
  revived_mgr.Reset(second_mgr.pid);
  EXPECT_EQ(second_mgr.internal_addr.Port(), cellappmgr_port);
  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_process_test",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > std::stoi(*first_acks);
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto second_restart_attempts =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/restart_attempts");
  ASSERT_TRUE(second_restart_attempts.has_value());
  EXPECT_EQ(*second_restart_attempts, "0");
  const auto second_generation =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/active_generation");
  ASSERT_TRUE(second_generation.has_value());
  EXPECT_EQ(std::stoull(*second_generation), std::stoull(*first_generation) + 1);
  const auto second_acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                             "reviver_process_test",
                                             "reviver/cellappmgr/heartbeat_acks");
  ASSERT_TRUE(second_acks.has_value());

  client.RequestShutdownTarget(ProcessType::kCellAppMgr, "cellappmgr_revived", 1);
  net.FlushDirtySendChannels();

  machined::ProcessInfo third_mgr;
  ASSERT_TRUE(WaitForNamedRegistrationWithDifferentPid(client, disp, ProcessType::kCellAppMgr,
                                                       "cellappmgr_revived", second_mgr.pid,
                                                       &third_mgr))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();
  revived_mgr.Reset(third_mgr.pid);
  EXPECT_EQ(third_mgr.internal_addr.Port(), cellappmgr_port);
  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_process_test",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > std::stoi(*second_acks);
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto third_generation =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_process_test",
                        "reviver/cellappmgr/active_generation");
  ASSERT_TRUE(third_generation.has_value());
  EXPECT_EQ(std::stoull(*third_generation), std::stoull(*second_generation) + 1);

  reviver = Child{};
  revived_mgr.Reset(0);
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
  std::filesystem::remove(reviver_config_path, ec);
  std::filesystem::remove(revived_output_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverAttachesToExistingCellAppMgrWithoutColdStart) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_attach", &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  Child existing_mgr = Child::Launch(
      cellappmgr_exe,
      {L"--type", L"cellappmgr", L"--name", L"cellappmgr_revived", L"--update-hertz",
       L"100", L"--internal-port", std::to_wstring(cellappmgr_port), L"--machined",
       machined_addr},
      "cellappmgr_existing");
  ASSERT_TRUE(existing_mgr.IsRunning()) << existing_mgr.Diagnostic();
  ASSERT_TRUE(WaitForUdpBound(cellappmgr_port, std::chrono::seconds(2)));

  EventDispatcher disp{"reviver_attach_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  machined::ProcessInfo existing_info;
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kCellAppMgr,
                                       "cellappmgr_revived", &existing_info))
      << machined.Diagnostic() << "\n"
      << existing_mgr.Diagnostic();
  ASSERT_NE(existing_info.pid, 0u);
  EXPECT_EQ(existing_info.internal_addr.Port(), cellappmgr_port);

  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_attach_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_attach_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  Child reviver = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_attach");
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver, "reviver_attach"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto active = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_attach", "reviver/cellappmgr/active");
    const auto active_pid = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                              "reviver_attach",
                                              "reviver/cellappmgr/active_pid");
    if (!active || !active_pid) return false;
    return *active == "true" && std::stoul(*active_pid) == existing_info.pid;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  const auto launch_count = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                              "reviver_attach",
                                              "reviver/cellappmgr/launch_count");
  ASSERT_TRUE(launch_count.has_value());
  EXPECT_EQ(*launch_count, "0");
  const auto active_generation = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                   "reviver_attach",
                                                   "reviver/cellappmgr/active_generation");
  ASSERT_TRUE(active_generation.has_value());
  EXPECT_EQ(*active_generation, "1");

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_attach",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto heartbeat_timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                    "reviver_attach",
                                                    "reviver/cellappmgr/heartbeat_timeouts");
  ASSERT_TRUE(heartbeat_timeouts.has_value());
  EXPECT_EQ(*heartbeat_timeouts, "0");

  auto managers = client.QuerySync(ProcessType::kCellAppMgr, std::chrono::milliseconds(200));
  int target_count = 0;
  for (const auto& info : managers) {
    if (info.name != "cellappmgr_revived") continue;
    ++target_count;
    EXPECT_EQ(info.pid, existing_info.pid);
  }
  EXPECT_EQ(target_count, 1);

  reviver = Child{};
  existing_mgr = Child{};
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverStartsNewCellAppMgrAfterTargetDiedBeforeSubscribe) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_missed_death",
                                      &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  Child dead_mgr = Child::Launch(
      cellappmgr_exe,
      {L"--type", L"cellappmgr", L"--name", L"cellappmgr_revived", L"--update-hertz",
       L"100", L"--internal-port", std::to_wstring(cellappmgr_port), L"--machined",
       machined_addr},
      "cellappmgr_dead_before_reviver");
  ASSERT_TRUE(dead_mgr.IsRunning()) << dead_mgr.Diagnostic();
  ASSERT_TRUE(WaitForUdpBound(cellappmgr_port, std::chrono::seconds(2)));

  EventDispatcher disp{"reviver_missed_death_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  machined::ProcessInfo dead_info;
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kCellAppMgr,
                                       "cellappmgr_revived", &dead_info))
      << machined.Diagnostic() << "\n"
      << dead_mgr.Diagnostic();
  ASSERT_NE(dead_info.pid, 0u);
  ASSERT_TRUE(TerminatePid(dead_info.pid));
  dead_mgr = Child{};

  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_missed_death_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_missed_death_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  PidGuard revived_mgr;
  Child reviver = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_missed_death");
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver,
                                       "reviver_missed_death"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  machined::ProcessInfo revived_info;
  ASSERT_TRUE(WaitForNamedRegistrationWithDifferentPid(client, disp, ProcessType::kCellAppMgr,
                                                       "cellappmgr_revived", dead_info.pid,
                                                       &revived_info))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();
  revived_mgr.Reset(revived_info.pid);
  EXPECT_EQ(revived_info.internal_addr.Port(), cellappmgr_port);

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto launch_count = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                "reviver_missed_death",
                                                "reviver/cellappmgr/launch_count");
    const auto active_pid = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                              "reviver_missed_death",
                                              "reviver/cellappmgr/active_pid");
    if (!launch_count || !active_pid) return false;
    return std::stoi(*launch_count) > 0 && std::stoul(*active_pid) == revived_info.pid;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_missed_death",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto heartbeat_timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                    "reviver_missed_death",
                                                    "reviver/cellappmgr/heartbeat_timeouts");
  ASSERT_TRUE(heartbeat_timeouts.has_value());
  EXPECT_EQ(*heartbeat_timeouts, "0");

  reviver = Child{};
  revived_mgr.Reset(0);
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverRestartsCellAppMgrWhenDirectHeartbeatDoesNotAck) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_heartbeat",
                                      &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  Child fake_mgr = Child::Launch(
      machined_exe,
      {L"--type", L"cellappmgr", L"--name", L"cellappmgr_revived", L"--update-hertz",
       L"100", L"--internal-port", std::to_wstring(cellappmgr_port), L"--machined",
       machined_addr},
      "cellappmgr_no_health_probe");
  ASSERT_TRUE(fake_mgr.IsRunning()) << fake_mgr.Diagnostic();
  ASSERT_TRUE(WaitForUdpBound(cellappmgr_port, std::chrono::seconds(2)));

  EventDispatcher disp{"reviver_heartbeat_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  machined::ProcessInfo fake_info;
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kCellAppMgr,
                                       "cellappmgr_revived", &fake_info))
      << machined.Diagnostic() << "\n"
      << fake_mgr.Diagnostic();
  ASSERT_NE(fake_info.pid, 0u);
  EXPECT_EQ(fake_info.internal_addr.Port(), cellappmgr_port);

  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_heartbeat_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_heartbeat_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  PidGuard revived_mgr;
  Child reviver = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_heartbeat");
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver,
                                       "reviver_heartbeat"))
      << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_heartbeat",
                                            "reviver/cellappmgr/heartbeat_timeouts");
    const auto failures = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_heartbeat",
                                            "reviver/cellappmgr/heartbeat_failures");
    const auto forced = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_heartbeat",
                                          "reviver/cellappmgr/forced_terminations");
    if (!timeouts || !failures || !forced) return false;
    return std::stoi(*timeouts) >= 2 && std::stoi(*failures) >= 2 &&
           std::stoi(*forced) >= 1;
  }, std::chrono::milliseconds(8000))) << machined.Diagnostic() << "\n"
      << fake_mgr.Diagnostic() << "\n"
      << reviver.Diagnostic();

  machined::ProcessInfo revived_info;
  ASSERT_TRUE(WaitForNamedRegistrationWithDifferentPid(client, disp, ProcessType::kCellAppMgr,
                                                       "cellappmgr_revived", fake_info.pid,
                                                       &revived_info))
      << machined.Diagnostic() << "\n"
      << fake_mgr.Diagnostic() << "\n"
      << reviver.Diagnostic();
  revived_mgr.Reset(revived_info.pid);
  EXPECT_EQ(revived_info.internal_addr.Port(), cellappmgr_port);

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto launch_count = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                "reviver_heartbeat",
                                                "reviver/cellappmgr/launch_count");
    const auto active_pid = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                              "reviver_heartbeat",
                                              "reviver/cellappmgr/active_pid");
    if (!launch_count || !active_pid) return false;
    return std::stoi(*launch_count) > 0 && std::stoul(*active_pid) == revived_info.pid;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_heartbeat",
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > 0;
  })) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto restart_attempts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                  "reviver_heartbeat",
                                                  "reviver/cellappmgr/restart_attempts");
  ASSERT_TRUE(restart_attempts.has_value());
  EXPECT_EQ(*restart_attempts, "0");
  const auto manager_health_failures =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_heartbeat",
                        "reviver/cellappmgr/manager_health_failures");
  ASSERT_TRUE(manager_health_failures.has_value());
  EXPECT_EQ(*manager_health_failures, "0");
  const auto manager_health_timeouts =
      QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_heartbeat",
                        "reviver/cellappmgr/manager_health_timeouts");
  ASSERT_TRUE(manager_health_timeouts.has_value());
  EXPECT_EQ(*manager_health_timeouts, "0");

  reviver = Child{};
  fake_mgr = Child{};
  revived_mgr.Reset(0);
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverTimesOutPendingManagerHealthWatcher) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_manager_timeout",
                                      &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  EventDispatcher registry_disp{"reviver_manager_timeout_registry"};
  registry_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface registry_net(registry_disp);
  MachinedClient client(registry_disp, registry_net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  EventDispatcher fake_disp{"reviver_manager_timeout_fake"};
  fake_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface fake_net(fake_disp);
  auto fake_channel = fake_net.ConnectTcp(Address("127.0.0.1", machined_port));
  ASSERT_TRUE(fake_channel.HasValue()) << fake_channel.Error().Message();

  machined::RegisterMessage reg;
  reg.process_type = ProcessType::kCellAppMgr;
  reg.name = "cellappmgr_revived";
  reg.internal_port = cellappmgr_port;
  reg.pid = ::GetCurrentProcessId();
  ASSERT_TRUE((*fake_channel)->SendMessage(reg).HasValue());
  fake_net.FlushDirtySendChannels();

  ASSERT_TRUE(PollUntil(registry_disp, fake_disp, [&] {
    fake_net.FlushDirtySendChannels();
    auto infos = client.QuerySync(ProcessType::kCellAppMgr, std::chrono::milliseconds(200));
    for (const auto& info : infos) {
      if (info.name == "cellappmgr_revived" && info.pid == reg.pid) return true;
    }
    return false;
  })) << machined.Diagnostic();

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_manager_timeout_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_manager_timeout_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  Child reviver = LaunchReviver(reviver_exe, machined_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_manager_timeout", 3, 20, 500);
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();
  ASSERT_TRUE(WaitForNamedRegistration(client, registry_disp, ProcessType::kReviver,
                                       "reviver_manager_timeout"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(registry_disp, [&] {
    const auto timeouts = QueryWatcherValue(client, registry_disp, ProcessType::kReviver,
                                            "reviver_manager_timeout",
                                            "reviver/cellappmgr/manager_health_timeouts");
    const auto failures = QueryWatcherValue(client, registry_disp, ProcessType::kReviver,
                                            "reviver_manager_timeout",
                                            "reviver/cellappmgr/manager_health_failures");
    if (!timeouts || !failures) return false;
    return std::stoi(*timeouts) >= 1 && std::stoi(*failures) >= 1;
  }, std::chrono::milliseconds(4000))) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  const auto forced = QueryWatcherValue(client, registry_disp, ProcessType::kReviver,
                                        "reviver_manager_timeout",
                                        "reviver/cellappmgr/forced_terminations");
  ASSERT_TRUE(forced.has_value());
  EXPECT_EQ(*forced, "0");
  const auto launches = QueryWatcherValue(client, registry_disp, ProcessType::kReviver,
                                          "reviver_manager_timeout",
                                          "reviver/cellappmgr/launch_count");
  ASSERT_TRUE(launches.has_value());
  EXPECT_EQ(*launches, "0");

  reviver = Child{};
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverStopsAfterRestartLimitWithoutHeartbeatAck) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_limit",
                                      &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_limit_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_limit_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  Child reviver = LaunchReviver(reviver_exe, machined_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_limit", 2);
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();

  EventDispatcher disp{"reviver_limit_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver, "reviver_limit"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto reached = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                           "reviver_limit",
                                           "reviver/cellappmgr/restart_limit_reached");
    const auto hits = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_limit",
                                        "reviver/cellappmgr/restart_limit_hits");
    const auto attempts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_limit",
                                            "reviver/cellappmgr/restart_attempts");
    const auto launches = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_limit",
                                            "reviver/cellappmgr/launch_count");
    if (!reached || !hits || !attempts || !launches) return false;
    return *reached == "true" && std::stoi(*hits) == 1 && std::stoi(*attempts) == 2 &&
           std::stoi(*launches) == 2;
  }, std::chrono::milliseconds(10000))) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const auto launch_count = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                              "reviver_limit",
                                              "reviver/cellappmgr/launch_count");
  ASSERT_TRUE(launch_count.has_value());
  EXPECT_EQ(*launch_count, "2");
  const auto reached = QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_limit",
                                         "reviver/cellappmgr/restart_limit_reached");
  ASSERT_TRUE(reached.has_value());
  EXPECT_EQ(*reached, "true");
  const auto status = QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_limit",
                                        "reviver/cellappmgr/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, "restart_limited");
  const auto launch_pending = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                "reviver_limit",
                                                "reviver/cellappmgr/launch_pending");
  ASSERT_TRUE(launch_pending.has_value());
  EXPECT_EQ(*launch_pending, "false");

  reviver = Child{};
  for (const auto& info : client.QuerySync(ProcessType::kCellAppMgr,
                                           std::chrono::milliseconds(200))) {
    if (info.name == "cellappmgr_revived" && info.pid != 0) {
      (void)TerminatePid(info.pid);
    }
  }
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverStopsAfterLaunchedProcessDoesNotRegister) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto atlas_tool_exe = ResolveServerExe(L"atlas_tool", L"atlas_tool.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || atlas_tool_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_launch_timeout",
                                      &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_launch_timeout_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_launch_timeout_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  Child reviver = LaunchReviver(reviver_exe, atlas_tool_exe, machined_addr,
                                cellappmgr_port, snapshot_path, leader_lock_path,
                                L"reviver_launch_timeout", 2, 2, 5000, 300);
  ASSERT_TRUE(reviver.IsRunning()) << reviver.Diagnostic();

  EventDispatcher disp{"reviver_launch_timeout_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver,
                                       "reviver_launch_timeout"))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto reached = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                           "reviver_launch_timeout",
                                           "reviver/cellappmgr/restart_limit_reached");
    const auto timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_launch_timeout",
                                            "reviver/cellappmgr/launch_timeouts");
    const auto failures = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_launch_timeout",
                                            "reviver/cellappmgr/launch_failures");
    const auto launches = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_launch_timeout",
                                            "reviver/cellappmgr/launch_count");
    if (!reached || !timeouts || !failures || !launches) return false;
    return *reached == "true" && std::stoi(*timeouts) == 2 &&
           std::stoi(*failures) >= 2 && std::stoi(*launches) == 2;
  }, std::chrono::milliseconds(8000))) << machined.Diagnostic() << "\n"
      << reviver.Diagnostic();
  const auto status = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        "reviver_launch_timeout",
                                        "reviver/cellappmgr/status");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, "restart_limited");
  const auto launch_pending = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                "reviver_launch_timeout",
                                                "reviver/cellappmgr/launch_pending");
  ASSERT_TRUE(launch_pending.has_value());
  EXPECT_EQ(*launch_pending, "false");

  reviver = Child{};
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}

TEST(CellAppMgrProcess, ReviverLeaderLockAllowsOnlyOneColdStartOwner) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only process harness";
#else
  const auto machined_exe = ResolveServerExe(L"machined", L"machined.exe");
  const auto cellappmgr_exe = ResolveServerExe(L"cellappmgr", L"atlas_cellappmgr.exe");
  const auto reviver_exe = ResolveServerExe(L"reviver", L"atlas_reviver.exe");
  if (machined_exe.empty() || cellappmgr_exe.empty() || reviver_exe.empty()) {
    GTEST_SKIP() << "server binaries not found; build_root=" << BuildRoot();
  }

  uint16_t machined_port = 0;
  Child machined;
  ASSERT_TRUE(LaunchMachinedWithRetry(machined_exe, L"reviver_lock", &machined_port, &machined))
      << "machined failed to start + bind TCP on any attempt";

  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(cellappmgr_port, 0u);
  const auto stamp =
      std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(::GetTickCount64());
  const auto snapshot_path = std::filesystem::temp_directory_path() /
                             ("atlas_reviver_lock_snapshot_" + stamp + ".bin");
  const auto leader_lock_path = std::filesystem::temp_directory_path() /
                                ("atlas_reviver_lock_" + stamp + ".lock");
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);

  PidGuard revived_mgr;
  Child reviver_a = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                  cellappmgr_port, snapshot_path, leader_lock_path,
                                  L"reviver_lock_a");
  Child reviver_b = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                  cellappmgr_port, snapshot_path, leader_lock_path,
                                  L"reviver_lock_b");
  ASSERT_TRUE(reviver_a.IsRunning()) << reviver_a.Diagnostic();
  ASSERT_TRUE(reviver_b.IsRunning()) << reviver_b.Diagnostic();

  EventDispatcher disp{"reviver_leader_lock_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver, "reviver_lock_a"))
      << machined.Diagnostic() << "\n" << reviver_a.Diagnostic();
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kReviver, "reviver_lock_b"))
      << machined.Diagnostic() << "\n" << reviver_b.Diagnostic();

  machined::ProcessInfo mgr;
  ASSERT_TRUE(WaitForNamedRegistration(client, disp, ProcessType::kCellAppMgr,
                                       "cellappmgr_revived", &mgr))
      << machined.Diagnostic() << "\n"
      << reviver_a.Diagnostic() << "\n"
      << reviver_b.Diagnostic();
  revived_mgr.Reset(mgr.pid);
  EXPECT_EQ(mgr.internal_addr.Port(), cellappmgr_port);

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto a = QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_lock_a",
                                     "reviver/leader/active");
    const auto b = QueryWatcherValue(client, disp, ProcessType::kReviver, "reviver_lock_b",
                                     "reviver/leader/active");
    if (!a || !b) return false;
    return (*a == "true" && *b == "false") || (*a == "false" && *b == "true");
  })) << machined.Diagnostic() << "\n"
      << reviver_a.Diagnostic() << "\n"
      << reviver_b.Diagnostic();

  const auto launches_a = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_lock_a",
                                            "reviver/cellappmgr/launch_count");
  const auto launches_b = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                            "reviver_lock_b",
                                            "reviver/cellappmgr/launch_count");
  ASSERT_TRUE(launches_a.has_value());
  ASSERT_TRUE(launches_b.has_value());
  EXPECT_EQ(std::stoi(*launches_a) + std::stoi(*launches_b), 1);

  const auto active_a = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_lock_a", "reviver/leader/active");
  const auto active_b = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          "reviver_lock_b", "reviver/leader/active");
  ASSERT_TRUE(active_a.has_value());
  ASSERT_TRUE(active_b.has_value());
  const bool a_was_leader = *active_a == "true";
  const std::string standby_name = a_was_leader ? "reviver_lock_b" : "reviver_lock_a";
  const int standby_launches_before = std::stoi(a_was_leader ? *launches_b : *launches_a);
  if (a_was_leader) {
    reviver_a = Child{};
  } else {
    reviver_b = Child{};
  }

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto active = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                          standby_name, "reviver/leader/active");
    return active && *active == "true";
  }, std::chrono::milliseconds(8000))) << machined.Diagnostic() << "\n"
      << (a_was_leader ? reviver_b.Diagnostic() : reviver_a.Diagnostic());

  const auto standby_launches_after = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                        standby_name,
                                                        "reviver/cellappmgr/launch_count");
  ASSERT_TRUE(standby_launches_after.has_value());
  EXPECT_EQ(std::stoi(*standby_launches_after), standby_launches_before);

  ASSERT_TRUE(PollUntil(disp, [&] {
    const auto acks = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                        standby_name,
                                        "reviver/cellappmgr/heartbeat_acks");
    if (!acks) return false;
    return std::stoi(*acks) > 0;
  }, std::chrono::milliseconds(8000))) << machined.Diagnostic() << "\n"
      << (a_was_leader ? reviver_b.Diagnostic() : reviver_a.Diagnostic());
  const auto heartbeat_timeouts = QueryWatcherValue(client, disp, ProcessType::kReviver,
                                                    standby_name,
                                                    "reviver/cellappmgr/heartbeat_timeouts");
  ASSERT_TRUE(heartbeat_timeouts.has_value());
  EXPECT_EQ(*heartbeat_timeouts, "0");

  auto mgr_infos = client.QuerySync(ProcessType::kCellAppMgr, std::chrono::milliseconds(200));
  int target_mgr_count = 0;
  for (const auto& info : mgr_infos) {
    if (info.name != "cellappmgr_revived") continue;
    ++target_mgr_count;
    EXPECT_EQ(info.pid, mgr.pid);
  }
  EXPECT_EQ(target_mgr_count, 1);

  reviver_b = Child{};
  reviver_a = Child{};
  revived_mgr.Reset(0);
  std::filesystem::remove(snapshot_path, ec);
  std::filesystem::remove(leader_lock_path, ec);
#endif
}
