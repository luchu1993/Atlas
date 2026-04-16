#if ATLAS_PLATFORM_LINUX

#include <dlfcn.h>

#include "platform/dynamic_library.h"

namespace atlas {

// ============================================================================
// Lifetime
// ============================================================================

DynamicLibrary::~DynamicLibrary() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Unload();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

// ============================================================================
// Loading
// ============================================================================

auto DynamicLibrary::Load(const std::filesystem::path& path) -> Result<DynamicLibrary> {
  void* handle = dlopen(path.string().c_str(), RTLD_NOW);
  if (!handle) {
    const char* err = dlerror();
    return Error{ErrorCode::kIoError,
                 std::string("dlopen failed: ") + (err ? err : "unknown error")};
  }
  return DynamicLibrary{handle};
}

// ============================================================================
// Symbol lookup
// ============================================================================

auto DynamicLibrary::GetSymbolRaw(std::string_view name) -> void* {
  if (!handle_) {
    return nullptr;
  }
  std::string name_str(name);
  return dlsym(handle_, name_str.c_str());
}

// ============================================================================
// Unload
// ============================================================================

void DynamicLibrary::Unload() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
