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

// Must match Atlas.Core.BootstrapArgs (C# [StructLayout(Sequential)]).
struct ClrBootstrapArgs {
  void (*error_set)(int32_t, const char*, int32_t){&ClrErrorSet};

  void (*error_clear)(){&ClrErrorClear};

  int32_t (*error_get_code)(){&ClrErrorGetCode};
};

static_assert(sizeof(ClrBootstrapArgs) == 24,
              "ClrBootstrapArgs layout mismatch with C# BootstrapArgs");

// Must match Atlas.Core.ObjectVTableOut (C# [StructLayout(Sequential)]).
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

[[nodiscard]] auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll)
    -> Result<void>;

// Use explicit args when the caller has a separate atlas_clrscript TLS copy.
[[nodiscard]] auto ClrBootstrap(ClrHost& host, const std::filesystem::path& runtime_dll,
                                ClrBootstrapArgs args) -> Result<void>;

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_BOOTSTRAP_H_
