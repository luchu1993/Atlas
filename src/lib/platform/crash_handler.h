#ifndef ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_
#define ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_

#include <functional>
#include <string>

#include "platform/platform_config.h"

namespace atlas {

// ============================================================================
// CrashHandler
// ============================================================================
//
// Captures a post-mortem dump when the process terminates abnormally and
// writes it to disk for offline debugging.
//
//   Windows: SetUnhandledExceptionFilter + dbghelp!MiniDumpWriteDump.
//            Also intercepts SIGABRT, pure-virtual calls and CRT invalid-
//            parameter callbacks, since none of those reach the SEH filter.
//
//   Linux:   sigaction(SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL) on an alternate
//            signal stack.  Writes a textual crash report (signal info +
//            backtrace) — full minidumps require breakpad/crashpad and are
//            out of scope here.
//
// The handler is best-effort: by definition the process state is corrupt
// when the handler runs, so we keep the write path allocation-free and
// rely only on pre-resolved syscalls / function pointers.

struct CrashHandlerOptions {
  // Process tag baked into the dump filename, e.g. "cellapp" or "baseapp".
  std::string process_name;

  // Output directory.  Created if missing.  Each crash produces one file
  // named "<process>_<pid>_<YYYYMMDD-HHMMSS>.<ext>" — .dmp on Windows,
  // .crash on Linux.
  std::string dump_dir;

  // Windows-only: when true, write a MiniDumpWithFullMemory dump (gigabytes
  // for large heaps).  Default writes MiniDumpWithDataSegs +
  // MiniDumpWithIndirectlyReferencedMemory + MiniDumpWithThreadInfo, which
  // is usually sufficient for stack/heap inspection at a few MB.
  bool full_memory = false;

  // Optional best-effort callback fired *after* the dump is written and
  // *before* the process is terminated.  Useful for flushing the logger
  // or printing the dump path.  Must not allocate or take locks the
  // crashing thread might already hold.
  std::function<void(const std::string& dump_path)> on_crash;
};

// Installs the platform crash handler.  Returns false if the dump
// directory could not be created or a platform call failed.
bool InstallCrashHandler(const CrashHandlerOptions& opts);

// Convenience wrapper for server processes.  Equivalent to building a
// CrashHandlerOptions with:
//   - process_name from the argument
//   - dump_dir   = $ATLAS_DUMP_DIR  (defaults to ".tmp/dumps")
//   - full_memory = $ATLAS_DUMP_FULL == "1"
//   - on_crash   = prints "<process>: crash dump written: <path>" to stderr
// Each app's main() can call this with a single line.
bool InstallDefaultCrashHandler(const std::string& process_name);

// Restores default handlers.  Mainly for tests; production code can leave
// the handler installed for the lifetime of the process.
void UninstallCrashHandler();

// Triggers a synthetic crash dump using the currently-installed handler,
// without actually crashing.  Returns the path of the written dump, or an
// empty string on failure.  Useful for smoke-testing dump output.
std::string WriteCrashDumpForTesting();

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_
