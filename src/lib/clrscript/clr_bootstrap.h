#ifndef ATLAS_LIB_CLRSCRIPT_CLR_BOOTSTRAP_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_BOOTSTRAP_H_

#include <cstdint>
#include <filesystem>

#include "clrscript/clr_error.h"
#include "clrscript/clr_host.h"
#include "clrscript/clr_invoke.h"
#include "clrscript/clr_object.h"
#include "foundation/error.h"

namespace atlas {

// ============================================================================
// ClrBootstrapArgs — passed from C++ to C# Bootstrap.Initialize()
// ============================================================================
//
// Must match Atlas.Core.BootstrapArgs (C# [StructLayout(Sequential)]) byte-for-byte.
//
// Layout (x64, each member = 8 bytes pointer):
//   offset  0: error_set      — void(*)(int, byte*, int)
//   offset  8: error_clear    — void(*)()
//   offset 16: error_get_code — int(*)()
//   sizeof = 24

struct ClrBootstrapArgs {
  // clr_error_set(int error_code, const char* msg, int msg_len)
  void (*error_set)(int32_t, const char*, int32_t){&ClrErrorSet};

  // clr_error_clear()
  void (*error_clear)(){&ClrErrorClear};

  // clr_error_get_code() → int
  int32_t (*error_get_code)(){&ClrErrorGetCode};
};

static_assert(sizeof(ClrBootstrapArgs) == 24,
              "ClrBootstrapArgs layout mismatch with C# BootstrapArgs");

// ============================================================================
// ClrObjectVTableOut — written by C# Bootstrap.Initialize(), read by C++
// ============================================================================
//
// Must match Atlas.Core.ObjectVTableOut (C# [StructLayout(Sequential)]) byte-for-byte.
//
// Each member is an 8-byte pointer (x64).
// sizeof = 7 * 8 = 56

struct ClrObjectVTableOut {
  void (*free_handle)(void*){nullptr};
  int32_t (*get_type_name)(void*, char*, int32_t){nullptr};
  uint8_t (*is_none)(void*){nullptr};
  int32_t (*to_int64)(void*, int64_t*){nullptr};
  int32_t (*to_double)(void*, double*){nullptr};
  int32_t (*to_string)(void*, char*, int32_t){nullptr};
  int32_t (*to_bool)(void*, uint8_t*){nullptr};
};

static_assert(sizeof(ClrObjectVTableOut) == 56,
              "ClrObjectVTableOut layout mismatch with C# ObjectVTableOut");

// ============================================================================
// clr_bootstrap() — call after ClrHost::initialize()
// ============================================================================
//
// 1. Locates Bootstrap.Initialize() in the Atlas.ClrHost assembly, which is
//    transitively loaded via the given script assembly (Atlas.Runtime for
//    server-side hosts, Atlas.Client for desktop-client hosts).
// 2. Constructs ClrBootstrapArgs with the C++ TLS error-bridge function pointers.
// 3. Calls Bootstrap.Initialize() which:
//      a. Registers ErrorBridge with the C++ error-bridge functions.
//      b. Fills ClrObjectVTableOut with GCHandleHelper function pointers.
// 4. Transfers the vtable pointers into ClrObjectVTable via set_clr_object_vtable().
//
// Parameters:
//   host          — initialized ClrHost (must be alive for the duration of CLR use)
//   runtime_dll   — path to the game-layer script assembly (server script dll
//                   or Atlas.ClientSample.dll) that transitively references
//                   Atlas.ClrHost
//
// Returns Error if:
//   - Bootstrap.Initialize() cannot be resolved.
//   - Bootstrap.Initialize() returns -1 (check stderr for details).
//   - Any vtable function pointer is null after initialization.

// Default overload: uses the local module's clr_error_* functions.
// Correct for production (server binaries that don't separately link atlas_clrscript).
[[nodiscard]] auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll)
    -> Result<void>;

// Overload with explicit args: use when the calling module separately links
// atlas_clrscript (creating a duplicate TLS copy).  Pass the DLL-sourced
// function pointers obtained via AtlasGetClrError*Fn() so both C# and C++
// read/write the same TLS buffer inside the shared library.
[[nodiscard]] auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll,
                                ClrBootstrapArgs args) -> Result<void>;

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_BOOTSTRAP_H_
