#include "platform/crash_handler.h"

#if ATLAS_PLATFORM_WINDOWS

#include <new.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
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
std::terminate_handler g_prev_terminate = nullptr;
PVOID g_vectored_handle = nullptr;

// Single dump per process — vectored, top-level filter and synthetic paths
// can all race during teardown.  First one wins.
std::atomic<bool> g_dump_written{false};

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
  // Race-safe: only the first caller writes a dump.  Subsequent callers on
  // the same crash (e.g. SEH filter after our vectored handler already
  // dumped) get EXCEPTION_CONTINUE_SEARCH and let the OS proceed.
  bool expected = false;
  if (!g_dump_written.compare_exchange_strong(expected, true)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

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

const char* ExceptionCodeName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_INT_OVERFLOW:
      return "INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_BREAKPOINT:
      return "BREAKPOINT";
    case 0xC0000374:
      return "HEAP_CORRUPTION";
    case 0xC0000409:
      return "STACK_BUFFER_OVERRUN/__fastfail";
    case 0xC0000420:
      return "ASSERTION_FAILURE";
    case 0xC000041D:
      return "FATAL_USER_CALLBACK_EXCEPTION";
    case 0xE06D7363:
      return "C++ exception";
    default:
      return "unknown";
  }
}

// Logs a one-line exception summary plus a symbolic stack trace to stderr.
// Best-effort: we call SymInitialize with fInvadeProcess=TRUE so loaded
// modules are auto-discovered.  Symbols are not freed — we are crashing.
void LogExceptionAndStack(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return;

  const auto* rec = ep->ExceptionRecord;
  std::fprintf(stderr, "[%s] exception 0x%08lX (%s) at %p\n", g_process_name_buf,
               rec->ExceptionCode, ExceptionCodeName(rec->ExceptionCode), rec->ExceptionAddress);
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const ULONG_PTR op = rec->ExceptionInformation[0];
    const ULONG_PTR addr = rec->ExceptionInformation[1];
    const char* op_str = (op == 0) ? "read" : (op == 1) ? "write" : (op == 8) ? "DEP/exec" : "?";
    std::fprintf(stderr, "[%s]   %s at 0x%llx\n", g_process_name_buf, op_str,
                 static_cast<unsigned long long>(addr));
  }

  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  static bool sym_inited = false;
  if (!sym_inited) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);
    sym_inited = true;
  }

  // StackWalk64 mutates the context, so work on a copy.
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame{};
  DWORD machine;
#if defined(_M_X64)
  machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
#else
  machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = ctx.Eip;
  frame.AddrFrame.Offset = ctx.Ebp;
  frame.AddrStack.Offset = ctx.Esp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  std::fprintf(stderr, "[%s] stack trace (thread %lu):\n", g_process_name_buf,
               GetCurrentThreadId());

  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(machine, process, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) break;

    const DWORD64 addr = frame.AddrPC.Offset;

    char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    DWORD64 sym_disp = 0;
    const char* sym_name = SymFromAddr(process, addr, &sym_disp, sym) ? sym->Name : "??";

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_disp = 0;
    char file_line[512] = "";
    if (SymGetLineFromAddr64(process, addr, &line_disp, &line)) {
      std::snprintf(file_line, sizeof(file_line), " %s:%lu", line.FileName, line.LineNumber);
    }

    IMAGEHLP_MODULE64 mod{};
    mod.SizeOfStruct = sizeof(mod);
    const char* mod_name = SymGetModuleInfo64(process, addr, &mod) ? mod.ModuleName : "?";

    std::fprintf(stderr, "  #%-2d 0x%016llX  %s!%s+0x%llx%s\n", i,
                 static_cast<unsigned long long>(addr), mod_name, sym_name,
                 static_cast<unsigned long long>(sym_disp), file_line);
  }
  std::fflush(stderr);
}

void NotifyAndExit(const char* dump_path, EXCEPTION_POINTERS* ep) {
  // Stack first, dump second: if stack walking itself faults, we still
  // want the user to see at least the exception code line.
  LogExceptionAndStack(ep);

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
    NotifyAndExit(dump_path, ep);
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
    NotifyAndExit(dump_path, &ep);
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

// Vectored handler runs *before* SEH unwinding, so it sees fatal exceptions
// even when an inner SEH frame would otherwise swallow them — important for
// CLR-hosted code where CoreCLR installs its own filters higher in the chain
// and may FailFast or re-throw without ever reaching SetUnhandledExceptionFilter.
//
// We restrict to fatal codes; CLR-hosted processes raise C++ exceptions
// (0xE06D7363) and managed exceptions through SEH all the time, so we'd
// dump on every Console.WriteLine without the filter.
LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN — also __fastfail target
    case 0xC0000420:  // STATUS_ASSERTION_FAILURE
    case 0xC000041D:  // STATUS_FATAL_USER_CALLBACK_EXCEPTION
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }

  char dump_path[MAX_PATH];
  if (WriteDump(ep, dump_path, sizeof(dump_path)) == EXCEPTION_EXECUTE_HANDLER) {
    NotifyAndExit(dump_path, ep);
  }
  // Let SEH unwinding continue so the OS still produces the usual exit code.
  return EXCEPTION_CONTINUE_SEARCH;
}

// std::terminate replacement.  Default behaviour is abort(), which we'd
// catch via SIGABRT — but the CLR's hosting layer can replace the
// terminate handler.  Reinstalling ours guarantees we always dump on
// uncaught C++ exceptions regardless of who else messed with it.
void OnTerminate() noexcept {
  DumpFromSyntheticContext();
  // Restore default and re-invoke so the runtime terminates as usual.
  std::set_terminate(g_prev_terminate ? g_prev_terminate : std::abort);
  std::abort();
}

// atexit hook: confirms whether main() returned cleanly.  If a dump was
// written, this tells you the dumper ran *and* main also unwound — i.e.
// a non-fatal crash on a worker thread.  If no dump and this fires, the
// process exited normally.  If neither fires, the process was killed
// (TerminateProcess, fastfail without our hook, or external SIGKILL).
void OnAtExit() {
  std::fprintf(stderr, "[%s] atexit: process exiting cleanly (dump_written=%d)\n",
               g_process_name_buf, g_dump_written.load() ? 1 : 0);
  std::fflush(stderr);
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
  g_prev_terminate = std::set_terminate(OnTerminate);
  g_vectored_handle = AddVectoredExceptionHandler(/*first=*/1, VectoredHandler);
  std::atexit(OnAtExit);

  // Reserve a bit of stack so the SEH filter can run after a stack-overflow.
  ULONG stack_guarantee = 64 * 1024;
  SetThreadStackGuarantee(&stack_guarantee);

  std::fprintf(stderr, "[%s] crash handler installed (dump_dir=%s)\n", g_process_name_buf,
               g_dump_dir_buf);
  std::fflush(stderr);

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
  if (g_prev_terminate) std::set_terminate(g_prev_terminate);
  if (g_vectored_handle) {
    RemoveVectoredExceptionHandler(g_vectored_handle);
    g_vectored_handle = nullptr;
  }
  // Reset the dump-written latch so a subsequent Install (e.g. between
  // unit tests) can fire WriteDump again.  In production the handler is
  // never uninstalled, so this only matters for tests.
  g_dump_written.store(false);

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
