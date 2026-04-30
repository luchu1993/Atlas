#if ATLAS_PLATFORM_WINDOWS

#include <windows.h>

#include "platform/dynamic_library.h"

namespace atlas {

DynamicLibrary::~DynamicLibrary() {
  if (handle_) {
    FreeLibrary(static_cast<HMODULE>(handle_));
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

auto DynamicLibrary::Load(const std::filesystem::path& path) -> Result<DynamicLibrary> {
  HMODULE module = LoadLibraryW(path.wstring().c_str());
  if (!module) {
    DWORD err = GetLastError();
    return Error{ErrorCode::kIoError,
                 "LoadLibrary failed (error " + std::to_string(err) + "): " + path.string()};
  }
  return DynamicLibrary{static_cast<void*>(module)};
}

auto DynamicLibrary::GetSymbolRaw(std::string_view name) -> void* {
  if (!handle_) {
    return nullptr;
  }
  std::string name_str(name);
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name_str.c_str()));
}

void DynamicLibrary::Unload() {
  if (handle_) {
    FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = nullptr;
  }
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_WINDOWS
