// Phase 11 PR-6 review-fix S4 — multi-process CellAppMgr harness.
//
// Boots the actual `atlas_machined.exe` + `atlas_cellappmgr.exe`
// binaries via CreateProcessW (same pattern as test_login_flow.cpp),
// then drives them with synthetic CellApp-like clients over real RUDP.
// This exercises the real binaries — static init, argv parsing,
// machined registration, ManagerApp ctor → Init → RunLoop — not just
// in-process threads.
//
// Scope deliberately stops at cellappmgr. The full 2×atlas_cellapp
// scenario requires a healthy CLR bring-up (Atlas.Runtime.dll loaded
// via hostfxr); that layer is orthogonal to Phase 11 and is covered
// end-to-end by test_login_flow.cpp + follow-up work.

#include <atomic>
#include <chrono>
#include <filesystem>
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
  // Walk up from the test executable path looking for the CMake
  // build root — identified by `src/server/machined/Debug/machined.exe`
  // (guaranteed to exist when this test is running because it links
  // against atlas_server which depends on that build output).
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

// Resolve a server exe by searching both the unified `bin/Debug` layout
// (present when a deploy step copies binaries together) and the per-
// target `build/<cfg>/src/server/<name>/Debug/` layout that CMake emits
// by default. Returns an empty path if neither exists.
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

// Shared process harness — modelled on test_login_flow.cpp's ChildProcess
// but trimmed to what this test needs.
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
    auto log_file = std::filesystem::temp_directory_path() /
                    ("atlas_cellappmgr_process_" + proc_label + ".log");
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

// Wait for machined to report that a process of `type` listening on
// `advertised_port` has registered.
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

  const uint16_t machined_port = ReserveTcpPort();
  const uint16_t cellappmgr_port = ReserveUdpPort();
  ASSERT_NE(machined_port, 0u);
  ASSERT_NE(cellappmgr_port, 0u);
  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);

  auto machined =
      Child::Launch(machined_exe,
                    {L"--type", L"machined", L"--name", L"machined_cellappmgr_process_test",
                     L"--update-hertz", L"100", L"--internal-port", std::to_wstring(machined_port)},
                    "machined");
  ASSERT_TRUE(machined.IsRunning()) << machined.Diagnostic();

  auto cellappmgr = Child::Launch(
      cellappmgr_exe,
      {L"--type", L"cellappmgr", L"--name", L"cellappmgr_process_test", L"--update-hertz", L"100",
       L"--internal-port", std::to_wstring(cellappmgr_port), L"--machined", machined_addr},
      "cellappmgr");
  ASSERT_TRUE(cellappmgr.IsRunning()) << cellappmgr.Diagnostic();

  EventDispatcher disp{"cellappmgr_process_registry"};
  disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface net(disp);
  MachinedClient client(disp, net);
  ASSERT_TRUE(client.Connect(Address("127.0.0.1", machined_port)));

  ASSERT_TRUE(WaitForRegistration(client, disp, ProcessType::kCellAppMgr, cellappmgr_port))
      << "atlas_cellappmgr.exe did not register with machined — " << cellappmgr.Diagnostic();
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

  const uint16_t machined_port = ReserveTcpPort();
  const uint16_t cellappmgr_port = ReserveUdpPort();
  const std::wstring machined_addr = L"127.0.0.1:" + std::to_wstring(machined_port);

  auto machined =
      Child::Launch(machined_exe,
                    {L"--type", L"machined", L"--name", L"machined_cellapp_register",
                     L"--update-hertz", L"100", L"--internal-port", std::to_wstring(machined_port)},
                    "machined");
  ASSERT_TRUE(machined.IsRunning());

  auto cellappmgr = Child::Launch(
      cellappmgr_exe,
      {L"--type", L"cellappmgr", L"--name", L"cellappmgr_register_test", L"--update-hertz", L"100",
       L"--internal-port", std::to_wstring(cellappmgr_port), L"--machined", machined_addr},
      "cellappmgr");
  ASSERT_TRUE(cellappmgr.IsRunning());

  // Wait for cellappmgr to be reachable (registered + RUDP listening).
  EventDispatcher registry_disp{"registry_probe"};
  registry_disp.SetMaxPollWait(Milliseconds(1));
  NetworkInterface registry_net(registry_disp);
  MachinedClient registry_client(registry_disp, registry_net);
  ASSERT_TRUE(registry_client.Connect(Address("127.0.0.1", machined_port)));
  ASSERT_TRUE(WaitForRegistration(registry_client, registry_disp, ProcessType::kCellAppMgr,
                                  cellappmgr_port))
      << cellappmgr.Diagnostic();

  // Drive a synthetic CellApp-like register flow against the real
  // cellappmgr process. Success demonstrates that argv parsing,
  // ManagerApp::Init + RUDP server startup, handler registration, and
  // ack path all work in a standalone process.
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
      << "RegisterCellAppAck not received from real cellappmgr binary — "
      << cellappmgr.Diagnostic();
  EXPECT_TRUE(ack_copy.success);
  EXPECT_EQ(ack_copy.app_id, 1u);
#endif
}
