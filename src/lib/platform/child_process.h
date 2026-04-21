#ifndef ATLAS_LIB_PLATFORM_CHILD_PROCESS_H_
#define ATLAS_LIB_PLATFORM_CHILD_PROCESS_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas {

// ============================================================================
// ChildProcess — minimal cross-platform subprocess launcher with async stdout.
//
// Use case: end-to-end test harnesses that drive a real server cluster from
// one host process and observe script-side stdout events from several real
// client subprocesses. See world_stress `--script-clients` for the motivating
// caller.
//
// The class owns the child's lifetime: on destruction (or on reset), any
// still-running process is terminated and reaped so the harness cannot leak
// zombies between test phases. Move-only; copying a handle to a running
// process would double-free.
//
// Threading: one internal reader thread per child, scoped to the ChildProcess
// instance. TryReadStdoutLine / WaitForStdoutLine are thread-safe with each
// other (single-consumer pattern).
// ============================================================================

class ChildProcess {
 public:
  struct Options {
    std::filesystem::path exe;                // absolute or resolvable via PATH
    std::vector<std::string> args;            // argv[1..] (exe is argv[0])
    std::filesystem::path working_directory;  // empty → inherit
    // stdout is always captured. stderr is currently merged into stdout so
    // script-side Console.Error writes are observable too (the ClientSample
    // uses both). A future Options.capture_stderr_separate may split them.
  };

  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  [[nodiscard]] static auto Start(Options opts) -> Result<ChildProcess>;

  // Pop the oldest buffered stdout line, if any. Non-blocking. Returned
  // string has trailing CR/LF stripped. EOF is reported indirectly as
  // std::nullopt; callers distinguish "no line yet" from "child exited" via
  // IsRunning().
  [[nodiscard]] auto TryReadStdoutLine() -> std::optional<std::string>;

  // Block up to `timeout` waiting for one stdout line. Returns nullopt on
  // timeout.
  [[nodiscard]] auto WaitForStdoutLine(std::chrono::milliseconds timeout)
      -> std::optional<std::string>;

  // True until the child has exited AND we have reaped its status.
  [[nodiscard]] auto IsRunning() const -> bool;

  // Send a terminate signal (SIGTERM / TerminateProcess). Idempotent.
  void Kill();

  // Wait for the child to exit up to `timeout`. Returns the exit code on
  // success, or nullopt on timeout. On platforms where the exit code is not
  // representable as int (e.g. terminated by signal), returns 128+signo.
  [[nodiscard]] auto Wait(std::chrono::milliseconds timeout) -> std::optional<int>;

 private:
  struct Impl;
  ChildProcess();
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_CHILD_PROCESS_H_
