#include "platform/crash_handler.h"

#if ATLAS_PLATFORM_WINDOWS

#include <new.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace atlas {

namespace crash_internal {

extern std::mutex g_install_mutex;
extern CrashHandlerOptions g_opts;
extern bool g_installed;
extern char g_dump_dir_buf[512];
extern std::size_t g_dump_dir_len;
extern char g_process_name_buf[64];
bool PrepareDumpDir(const std::string& dir);
void FormatDumpStem(char* out, std::size_t out_size, int pid);

// Saved handlers so Uninstall can restore them.
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_seh_filter = nullptr;
_purecall_handler g_prev_purecall = nullptr;
_invalid_parameter_handler g_prev_invalid_param = nullptr;
void (*g_prev_sigabrt)(int) = SIG_DFL;

// Builds the absolute dump path into `out` (must be >= MAX_PATH).
void BuildDumpPath(char* out, std::size_t out_size) {
  char stem[128];
  FormatDumpStem(stem, sizeof(stem), static_cast<int>(GetCurrentProcessId()));
  std::snprintf(out, out_size, "%s%s.dmp", g_dump_dir_buf, stem);
}

// Performs the actual MiniDumpWriteDump call.  Called from the SEH filter
// (with real EXCEPTION_POINTERS) and from synthetic-context paths
// (SIGABRT, pure call, invalid parameter).
LONG WriteDump(EXCEPTION_POINTERS* ep, char* dump_path_out, std::size_t dump_path_size) {
  BuildDumpPath(dump_path_out, dump_path_size);

  HANDLE file = CreateFileA(dump_path_out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  MINIDUMP_EXCEPTION_INFORMATION info{};
  info.ThreadId = GetCurrentThreadId();
  info.ExceptionPointers = ep;
  info.ClientPointers = FALSE;

  MINIDUMP_TYPE type =
      g_opts.full_memory
          ? static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                       MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData)
          : static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs |
                                       MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

  BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                              ep ? &info : nullptr, nullptr, nullptr);
  CloseHandle(file);

  if (!ok) {
    DeleteFileA(dump_path_out);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

void NotifyAndExit(const char* dump_path) {
  if (!g_opts.on_crash) return;
  // C++ try/catch — cannot use __try here because std::function::operator()
  // constructs a std::string parameter, which requires object unwinding.
  // SEH coming back out of a misbehaving callback will still kill us, but
  // that's acceptable: the dump is already on disk.
  try {
    g_opts.on_crash(dump_path);
  } catch (...) {}
}

// Top-level SEH filter.  Triggered by access violations, stack overflow,
// divide-by-zero, RaiseException, etc.
LONG WINAPI SehFilter(EXCEPTION_POINTERS* ep) {
  char dump_path[MAX_PATH];
  LONG result = WriteDump(ep, dump_path, sizeof(dump_path));
  if (result == EXCEPTION_EXECUTE_HANDLER) {
    NotifyAndExit(dump_path);
  }
  return result;
}

// SIGABRT / pure-virtual / invalid-parameter all bypass the SEH filter,
// so we synthesize a context here.
void DumpFromSyntheticContext() {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  RtlCaptureContext(&ctx);

  EXCEPTION_RECORD rec{};
  rec.ExceptionCode = 0xE0000001;  // arbitrary user-defined code
  rec.ExceptionAddress = reinterpret_cast<PVOID>(&DumpFromSyntheticContext);

  EXCEPTION_POINTERS ep{&rec, &ctx};

  char dump_path[MAX_PATH];
  if (WriteDump(&ep, dump_path, sizeof(dump_path)) == EXCEPTION_EXECUTE_HANDLER) {
    NotifyAndExit(dump_path);
  }
}

void OnSigabrt(int /*sig*/) {
  DumpFromSyntheticContext();
  // Restore default and re-raise so the OS produces the usual exit code.
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

void OnPureCall() {
  DumpFromSyntheticContext();
  TerminateProcess(GetCurrentProcess(), 0xE0000002);
}

void OnInvalidParameter(const wchar_t* /*expression*/, const wchar_t* /*function*/,
                        const wchar_t* /*file*/, unsigned int /*line*/, uintptr_t /*reserved*/) {
  DumpFromSyntheticContext();
  TerminateProcess(GetCurrentProcess(), 0xE0000003);
}

}  // namespace crash_internal

// ============================================================================
// Public API
// ============================================================================

bool InstallCrashHandler(const CrashHandlerOptions& opts) {
  using namespace crash_internal;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (g_installed) {
    return true;
  }

  g_opts = opts;

  if (!PrepareDumpDir(opts.dump_dir)) {
    return false;
  }

  std::snprintf(g_process_name_buf, sizeof(g_process_name_buf), "%s",
                opts.process_name.empty() ? "process" : opts.process_name.c_str());

  g_prev_seh_filter = SetUnhandledExceptionFilter(SehFilter);
  g_prev_purecall = _set_purecall_handler(OnPureCall);
  g_prev_invalid_param = _set_invalid_parameter_handler(OnInvalidParameter);
  g_prev_sigabrt = std::signal(SIGABRT, OnSigabrt);

  // Reserve a bit of stack so the SEH filter can run after a stack-overflow.
  ULONG stack_guarantee = 64 * 1024;
  SetThreadStackGuarantee(&stack_guarantee);

  g_installed = true;
  return true;
}

void UninstallCrashHandler() {
  using namespace crash_internal;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (!g_installed) return;

  SetUnhandledExceptionFilter(g_prev_seh_filter);
  _set_purecall_handler(g_prev_purecall);
  _set_invalid_parameter_handler(g_prev_invalid_param);
  std::signal(SIGABRT, g_prev_sigabrt);

  g_installed = false;
}

std::string WriteCrashDumpForTesting() {
  using namespace crash_internal;
  if (!g_installed) return {};

  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  RtlCaptureContext(&ctx);

  EXCEPTION_RECORD rec{};
  rec.ExceptionCode = 0xE0000FFF;
  rec.ExceptionAddress = reinterpret_cast<PVOID>(&WriteCrashDumpForTesting);

  EXCEPTION_POINTERS ep{&rec, &ctx};

  char dump_path[MAX_PATH];
  if (WriteDump(&ep, dump_path, sizeof(dump_path)) != EXCEPTION_EXECUTE_HANDLER) {
    return {};
  }
  return std::string(dump_path);
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
