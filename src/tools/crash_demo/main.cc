// crash_demo — triggers real crashes to validate the platform crash
// handler end-to-end.  NOT a test target: each invocation kills the
// process by design.  Pick a mode via the first argv:
//
//   crash_demo segv          dereference a null pointer (SEH AV)
//   crash_demo abort         call std::abort()           (SIGABRT)
//   crash_demo divzero       integer divide by zero      (SEH on Windows)
//   crash_demo stackoverflow recurse forever             (SEH on alt stack)
//   crash_demo throw         throw an uncaught exception (terminate -> abort)
//   crash_demo purecall      call a pure virtual         (Windows only)
//   crash_demo test          synthetic dump, no crash    (smoke test)
//
// Dump dir defaults to .tmp/dumps; override with ATLAS_DUMP_DIR.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "platform/crash_handler.h"

namespace {

[[noreturn]] void TriggerSegv() {
  // Volatile so the optimizer can't elide the deref.
  volatile int* p = nullptr;
  *p = 0xdeadbeef;
  std::_Exit(99);  // unreachable
}

[[noreturn]] void TriggerAbort() {
  std::abort();
}

[[noreturn]] void TriggerDivZero() {
  // volatile to prevent constant folding into UB at compile time.
  volatile int z = 0;
  volatile int y = 1 / z;
  (void)y;
  std::_Exit(99);
}

void Recurse(int depth);
using RecurseFn = void (*)(int);
RecurseFn volatile recurse_fn = Recurse;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717)
#endif
void Recurse(int depth) {
  volatile char pad[1024];
  for (std::size_t i = 0; i < sizeof(pad); ++i) pad[i] = static_cast<char>(depth + i);
  recurse_fn(depth + 1);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
[[noreturn]] void TriggerStackOverflow() {
  Recurse(0);
  std::_Exit(99);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

[[noreturn]] void TriggerThrow() {
  throw std::runtime_error("crash_demo: uncaught exception");
}

#if defined(_WIN32)
struct PureBase {
  virtual void Foo() = 0;
  PureBase() { Call(); }
  void Call() { Foo(); }  // call before vtable is set up for derived
};
struct PureDerived : PureBase {
  void Foo() override {}
};

[[noreturn]] void TriggerPureCall() {
  // Construct a derived: PureBase ctor calls Foo() before the override is
  // installed, which routes through _purecall on MSVC.
  PureDerived d;
  (void)d;
  std::_Exit(99);
}
#endif

}  // namespace

int main(int argc, char* argv[]) {
  atlas::InstallDefaultCrashHandler("crash_demo");

  const char* mode = (argc > 1) ? argv[1] : "test";
  std::fprintf(stderr, "crash_demo: mode=%s\n", mode);
  std::fflush(stderr);

  if (std::strcmp(mode, "segv") == 0) TriggerSegv();
  if (std::strcmp(mode, "abort") == 0) TriggerAbort();
  if (std::strcmp(mode, "divzero") == 0) TriggerDivZero();
  if (std::strcmp(mode, "stackoverflow") == 0) TriggerStackOverflow();
  if (std::strcmp(mode, "throw") == 0) TriggerThrow();
#if defined(_WIN32)
  if (std::strcmp(mode, "purecall") == 0) TriggerPureCall();
#endif

  if (std::strcmp(mode, "test") == 0) {
    std::string path = atlas::WriteCrashDumpForTesting();
    if (path.empty()) {
      std::fprintf(stderr, "crash_demo: WriteCrashDumpForTesting failed\n");
      return 2;
    }
    std::fprintf(stderr, "crash_demo: synthetic dump written: %s\n", path.c_str());
    return 0;
  }

  std::fprintf(stderr, "crash_demo: unknown mode '%s'\n", mode);
  return 1;
}
