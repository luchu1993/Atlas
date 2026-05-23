#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
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
  if (std::filesystem::exists(p1)) return p1;
  auto p2 = BuildRoot() / "src" / "server" / subdir / "Debug" / filename;
  if (std::filesystem::exists(p2)) return p2;
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
                   const std::filesystem::path& snapshot_path) -> Child {
  return Child::Launch(
      reviver_exe,
      {L"--type", L"reviver", L"--name", L"reviver_process_test", L"--update-hertz", L"100",
       L"--machined", machined_addr, L"--revive-cellappmgr-exe", cellappmgr_exe.wstring(),
       L"--revive-cellappmgr-name", L"cellappmgr_revived", L"--revive-cellappmgr-port",
       std::to_wstring(cellappmgr_port), L"--revive-cellappmgr-on-start", L"true",
       L"--revive-cellappmgr-snapshot-path", snapshot_path.wstring(),
       L"--revive-cellappmgr-update-hertz", L"100", L"--revive-restart-delay-ms", L"50",
       L"--revive-max-restarts", L"3"},
      "reviver");
}

#endif  // defined(_WIN32)

}  // namespace

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
  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);

  PidGuard revived_mgr;
  Child reviver = LaunchReviver(reviver_exe, cellappmgr_exe, machined_addr,
                                cellappmgr_port, snapshot_path);
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

  ASSERT_TRUE(TerminatePid(first_mgr.pid));
  revived_mgr.Forget();

  machined::ProcessInfo second_mgr;
  ASSERT_TRUE(WaitForNamedRegistrationWithDifferentPid(client, disp, ProcessType::kCellAppMgr,
                                                       "cellappmgr_revived", first_mgr.pid,
                                                       &second_mgr))
      << machined.Diagnostic() << "\n" << reviver.Diagnostic();
  revived_mgr.Reset(second_mgr.pid);
  EXPECT_EQ(second_mgr.internal_addr.Port(), cellappmgr_port);

  std::filesystem::remove(snapshot_path, ec);
#endif
}
