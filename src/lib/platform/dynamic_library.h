#ifndef ATLAS_LIB_PLATFORM_DYNAMIC_LIBRARY_H_
#define ATLAS_LIB_PLATFORM_DYNAMIC_LIBRARY_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "foundation/error.h"

namespace atlas {

// ============================================================================
// DynamicLibrary
// ============================================================================

class DynamicLibrary {
 public:
  ~DynamicLibrary();

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

  [[nodiscard]] static auto Load(const std::filesystem::path& path) -> Result<DynamicLibrary>;

  template <typename FuncPtr>
  [[nodiscard]] auto GetSymbol(std::string_view name) -> Result<FuncPtr> {
    auto* sym = GetSymbolRaw(name);
    if (!sym) {
      return Error{ErrorCode::kNotFound, std::string("Symbol not found: ") + std::string(name)};
    }
    return reinterpret_cast<FuncPtr>(sym);
  }

  [[nodiscard]] auto IsLoaded() const -> bool { return handle_ != nullptr; }
  void Unload();

 private:
  DynamicLibrary() = default;
  explicit DynamicLibrary(void* handle) : handle_(handle) {}
  auto GetSymbolRaw(std::string_view name) -> void*;
  void* handle_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_DYNAMIC_LIBRARY_H_
