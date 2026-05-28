#include "process_launcher.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace atlas {
namespace {

auto QuoteArg(std::wstring_view arg) -> std::wstring {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring_view::npos) {
    return std::wstring(arg);
  }
  std::wstring out;
  out.reserve(arg.size() + 2);
  out.push_back(L'"');
  for (std::size_t i = 0; i < arg.size(); ++i) {
    std::size_t slash_count = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++slash_count;
      ++i;
    }
    if (i == arg.size()) {
      out.append(slash_count * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(slash_count * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(slash_count, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

auto Utf8ToWide(std::string_view value) -> std::wstring {
  if (value.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                      static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
  return out;
}

inline auto TokenToHandle(uint64_t token) -> HANDLE {
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token));
}

inline auto HandleToToken(HANDLE h) -> uint64_t {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h));
}

}  // namespace

LaunchedProcess::LaunchedProcess(LaunchedProcess&& other) noexcept
    : pid_(other.pid_), platform_token_(other.platform_token_) {
  other.pid_ = 0;
  other.platform_token_ = 0;
}

auto LaunchedProcess::operator=(LaunchedProcess&& other) noexcept -> LaunchedProcess& {
  if (this != &other) {
    Reset();
    pid_ = other.pid_;
    platform_token_ = other.platform_token_;
    other.pid_ = 0;
    other.platform_token_ = 0;
  }
  return *this;
}

LaunchedProcess::~LaunchedProcess() { Reset(); }

void LaunchedProcess::Reset() {
  if (platform_token_ != 0) {
    CloseHandle(TokenToHandle(platform_token_));
  }
  pid_ = 0;
  platform_token_ = 0;
}

auto LaunchedProcess::IsAlive() const -> bool {
  if (platform_token_ == 0) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(TokenToHandle(platform_token_), &code)) return false;
  return code == STILL_ACTIVE;
}

auto LaunchedProcess::Terminate() -> bool {
  if (platform_token_ == 0) return false;
  return TerminateProcess(TokenToHandle(platform_token_), 1) != FALSE;
}

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "LaunchDetachedProcess: exe path empty"};
  }

  HANDLE output_handle = INVALID_HANDLE_VALUE;
  HANDLE input_handle = INVALID_HANDLE_VALUE;
  if (!opts.output_path.empty()) {
    const auto parent = opts.output_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return Error{ErrorCode::kInternalError,
                     "create output directory failed: " + ec.message()};
      }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    output_handle = CreateFileW(
        opts.output_path.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output_handle == INVALID_HANDLE_VALUE) {
      return Error{ErrorCode::kInternalError,
                   "CreateFileW output failed, gle=" + std::to_string(GetLastError())};
    }
    SetFilePointer(output_handle, 0, nullptr, FILE_END);
    input_handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }

  std::wstring command_line = QuoteArg(opts.exe.wstring());
  for (const auto& arg : opts.args) {
    command_line.push_back(L' ');
    command_line += QuoteArg(Utf8ToWide(arg));
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  if (output_handle != INVALID_HANDLE_VALUE) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = input_handle != INVALID_HANDLE_VALUE ? input_handle : nullptr;
    si.hStdOutput = output_handle;
    si.hStdError = output_handle;
  }
  PROCESS_INFORMATION pi{};
  std::wstring exe = opts.exe.wstring();
  std::wstring cwd =
      opts.working_directory.empty() ? std::wstring{} : opts.working_directory.wstring();
  const LPCWSTR cwd_arg = cwd.empty() ? nullptr : cwd.c_str();
  const BOOL inherit_handles = output_handle != INVALID_HANDLE_VALUE ? TRUE : FALSE;
  if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr,
                      inherit_handles, CREATE_NEW_PROCESS_GROUP, nullptr, cwd_arg, &si, &pi)) {
    const auto gle = GetLastError();
    if (input_handle != INVALID_HANDLE_VALUE) CloseHandle(input_handle);
    if (output_handle != INVALID_HANDLE_VALUE) CloseHandle(output_handle);
    return Error{ErrorCode::kInternalError,
                 "CreateProcessW failed, gle=" + std::to_string(gle)};
  }

  const auto pid = static_cast<uint32_t>(pi.dwProcessId);
  CloseHandle(pi.hThread);
  if (input_handle != INVALID_HANDLE_VALUE) CloseHandle(input_handle);
  if (output_handle != INVALID_HANDLE_VALUE) CloseHandle(output_handle);
  // pi.hProcess transfers into LaunchedProcess; do not close here.
  return LaunchedProcess(pid, HandleToToken(pi.hProcess));
}

auto IsProcessAlive(uint32_t pid) -> bool {
  if (pid == 0) return false;
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (proc == nullptr) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
  CloseHandle(proc);
  return alive;
}

auto TerminateProcessByPid(uint32_t pid) -> bool {
  if (pid == 0) return false;
  HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (proc == nullptr) return false;
  const bool ok = TerminateProcess(proc, 1) != FALSE;
  CloseHandle(proc);
  return ok;
}

}  // namespace atlas
