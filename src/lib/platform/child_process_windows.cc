#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

#include "child_process.h"

namespace atlas {

namespace {

// Quote a single argv element per Microsoft C runtime rules — backslashes
// before a double quote double up, surrounding quotes wrap any token
// containing whitespace. Reference: "Parsing C Command-Line Arguments" in
// the Win32 docs.
auto QuoteArg(std::string_view arg) -> std::string {
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string_view::npos) {
    return std::string(arg);
  }
  std::string out;
  out.reserve(arg.size() + 2);
  out.push_back('"');
  for (std::size_t i = 0; i < arg.size(); ++i) {
    std::size_t bs = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++bs;
      ++i;
    }
    if (i == arg.size()) {
      out.append(bs * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(bs * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(bs, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

auto Utf8ToWide(std::string_view s) -> std::wstring {
  if (s.empty()) return {};
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), wlen);
  return w;
}

}  // namespace

struct ChildProcess::Impl {
  HANDLE process{nullptr};
  DWORD pid{0};
  HANDLE stdout_read{nullptr};
  std::thread reader;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> lines;
  std::string pending;  // partial line across reads
  bool reader_eof{false};

  std::atomic<bool> reaped{false};
  int cached_exit{0};
};

ChildProcess::ChildProcess() = default;
ChildProcess::ChildProcess(ChildProcess&&) noexcept = default;
ChildProcess& ChildProcess::operator=(ChildProcess&&) noexcept = default;

ChildProcess::~ChildProcess() {
  if (!impl_) return;
  if (impl_->process) {
    // Best-effort terminate; fine if it's already gone.
    TerminateProcess(impl_->process, 1);
    WaitForSingleObject(impl_->process, 2000);
    CloseHandle(impl_->process);
    impl_->process = nullptr;
  }
  if (impl_->stdout_read) {
    CloseHandle(impl_->stdout_read);
    impl_->stdout_read = nullptr;
  }
  if (impl_->reader.joinable()) impl_->reader.join();
}

auto ChildProcess::Start(Options opts) -> Result<ChildProcess> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "ChildProcess::Start: exe path empty"};
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 64 * 1024)) {
    return Error{ErrorCode::kInternalError,
                 "ChildProcess::Start: CreatePipe failed, gle=" + std::to_string(GetLastError())};
  }
  // Read end stays in parent and MUST NOT be inherited by the child —
  // otherwise the pipe never reports EOF.
  if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return Error{ErrorCode::kInternalError, "SetHandleInformation(read_end) failed"};
  }

  // Assemble the command line. CreateProcessW expects a mutable buffer.
  std::ostringstream cmd;
  cmd << QuoteArg(opts.exe.string());
  for (auto& a : opts.args) cmd << ' ' << QuoteArg(a);
  std::wstring wcmd = Utf8ToWide(cmd.str());
  std::wstring wexe = opts.exe.wstring();
  std::wstring wcwd =
      opts.working_directory.empty() ? std::wstring{} : opts.working_directory.wstring();

  // Open NUL as the child's stdin — safer than inheriting STD_INPUT_HANDLE,
  // which may be a broken handle when the parent itself was launched with a
  // redirected stdin (pipe from Python subprocess / bash, console-less
  // service, etc.). CoreCLR's runtime init touches console handles and
  // hangs or truncates output when stdin is in an odd state.
  SECURITY_ATTRIBUTES nul_sa{};
  nul_sa.nLength = sizeof(nul_sa);
  nul_sa.bInheritHandle = TRUE;
  HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &nul_sa,
                              OPEN_EXISTING, 0, nullptr);
  if (nul_in == INVALID_HANDLE_VALUE) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return Error{ErrorCode::kInternalError, "ChildProcess::Start: CreateFileW(NUL) failed, gle=" +
                                                std::to_string(GetLastError())};
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul_in;
  si.hStdOutput = write_end;
  si.hStdError = write_end;  // merge stderr into stdout — script writes to both

  PROCESS_INFORMATION pi{};
  const LPCWSTR cwd_arg = wcwd.empty() ? nullptr : wcwd.c_str();
  if (!CreateProcessW(wexe.c_str(), wcmd.data(), nullptr, nullptr,
                      /*inheritHandles=*/TRUE, 0, nullptr, cwd_arg, &si, &pi)) {
    const DWORD gle = GetLastError();
    CloseHandle(read_end);
    CloseHandle(write_end);
    CloseHandle(nul_in);
    return Error{ErrorCode::kInternalError, "CreateProcessW failed (gle=" + std::to_string(gle) +
                                                ") for exe=" + opts.exe.string()};
  }

  // Close the write end + NUL stdin in the parent so the pipe signals EOF
  // when the child exits. The child keeps its own inherited duplicates.
  CloseHandle(write_end);
  CloseHandle(nul_in);
  CloseHandle(pi.hThread);  // we never touch the primary thread separately

  ChildProcess cp;
  cp.impl_ = std::make_unique<Impl>();
  cp.impl_->process = pi.hProcess;
  cp.impl_->pid = pi.dwProcessId;
  cp.impl_->stdout_read = read_end;

  // Reader thread: ReadFile → split on '\n' → push to queue. Unix-style line
  // endings are the norm even on Windows for .NET Console.WriteLine in most
  // shells, but we strip trailing '\r' defensively.
  Impl* impl = cp.impl_.get();
  cp.impl_->reader = std::thread([impl] {
    char buf[4096];
    while (true) {
      DWORD n = 0;
      BOOL ok = ReadFile(impl->stdout_read, buf, sizeof(buf), &n, nullptr);
      if (!ok || n == 0) break;  // ERROR_BROKEN_PIPE on child exit
      std::lock_guard<std::mutex> lock(impl->mu);
      for (DWORD i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == '\n') {
          if (!impl->pending.empty() && impl->pending.back() == '\r') {
            impl->pending.pop_back();
          }
          impl->lines.emplace_back(std::move(impl->pending));
          impl->pending.clear();
        } else {
          impl->pending.push_back(c);
        }
      }
      impl->cv.notify_all();
    }
    std::lock_guard<std::mutex> lock(impl->mu);
    if (!impl->pending.empty()) {
      impl->lines.emplace_back(std::move(impl->pending));
      impl->pending.clear();
    }
    impl->reader_eof = true;
    impl->cv.notify_all();
  });

  return cp;
}

auto ChildProcess::TryReadStdoutLine() -> std::optional<std::string> {
  if (!impl_) return std::nullopt;
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->lines.empty()) return std::nullopt;
  auto s = std::move(impl_->lines.front());
  impl_->lines.pop_front();
  return s;
}

auto ChildProcess::WaitForStdoutLine(std::chrono::milliseconds timeout)
    -> std::optional<std::string> {
  if (!impl_) return std::nullopt;
  std::unique_lock<std::mutex> lock(impl_->mu);
  if (!impl_->cv.wait_for(lock, timeout,
                          [this] { return !impl_->lines.empty() || impl_->reader_eof; })) {
    return std::nullopt;
  }
  if (impl_->lines.empty()) return std::nullopt;
  auto s = std::move(impl_->lines.front());
  impl_->lines.pop_front();
  return s;
}

auto ChildProcess::IsRunning() const -> bool {
  if (!impl_ || !impl_->process) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(impl_->process, &code)) return false;
  return code == STILL_ACTIVE;
}

void ChildProcess::Kill() {
  if (!impl_ || !impl_->process) return;
  TerminateProcess(impl_->process, 1);
}

auto ChildProcess::Wait(std::chrono::milliseconds timeout) -> std::optional<int> {
  if (!impl_ || !impl_->process) return std::nullopt;
  if (impl_->reaped.load()) return impl_->cached_exit;
  DWORD wait_ms = timeout.count() > static_cast<long long>(INFINITE - 1)
                      ? INFINITE - 1
                      : static_cast<DWORD>(timeout.count());
  DWORD rc = WaitForSingleObject(impl_->process, wait_ms);
  if (rc != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  GetExitCodeProcess(impl_->process, &code);
  impl_->cached_exit = static_cast<int>(code);
  impl_->reaped.store(true);
  return impl_->cached_exit;
}

}  // namespace atlas
