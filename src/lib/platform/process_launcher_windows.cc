#include "process_launcher.h"

#include <windows.h>

#include <string>
#include <string_view>

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

}  // namespace

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<uint32_t> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "LaunchDetachedProcess: exe path empty"};
  }

  std::wstring command_line = QuoteArg(opts.exe.wstring());
  for (const auto& arg : opts.args) {
    command_line.push_back(L' ');
    command_line += QuoteArg(Utf8ToWide(arg));
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring exe = opts.exe.wstring();
  std::wstring cwd =
      opts.working_directory.empty() ? std::wstring{} : opts.working_directory.wstring();
  const LPCWSTR cwd_arg = cwd.empty() ? nullptr : cwd.c_str();
  constexpr BOOL kInheritHandles = FALSE;
  if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr,
                      kInheritHandles, CREATE_NEW_PROCESS_GROUP, nullptr, cwd_arg, &si, &pi)) {
    return Error{ErrorCode::kInternalError,
                 "CreateProcessW failed, gle=" + std::to_string(GetLastError())};
  }

  const auto pid = static_cast<uint32_t>(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return pid;
}

}  // namespace atlas
