#ifndef ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_
#define ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_

#include <functional>
#include <string>

#include "platform/platform_config.h"

namespace atlas {

// The handler is best-effort: by definition the process state is corrupt
// when the handler runs, so we keep the write path allocation-free and
// rely only on pre-resolved syscalls / function pointers.

struct CrashHandlerOptions {
  // Process tag baked into the dump filename, e.g. "cellapp" or "baseapp".
  std::string process_name;

  // Output directory. Each crash produces one timestamped .dmp or .crash file.
  std::string dump_dir;

  // Windows-only: full memory dumps can be gigabytes for large heaps.
  bool full_memory = false;

  // Optional best-effort callback fired *after* the dump is written and
  // *before* the process is terminated.  Useful for flushing the logger
  // or printing the dump path.  Must not allocate or take locks the
  // crashing thread might already hold.
  std::function<void(const std::string& dump_path)> on_crash;
};

bool InstallCrashHandler(const CrashHandlerOptions& opts);

bool InstallDefaultCrashHandler(const std::string& process_name);

void UninstallCrashHandler();

std::string WriteCrashDumpForTesting();

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_CRASH_HANDLER_H_
