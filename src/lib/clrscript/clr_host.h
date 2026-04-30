#ifndef ATLAS_LIB_CLRSCRIPT_CLR_HOST_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_HOST_H_

#include <filesystem>
#include <optional>
#include <string_view>
#include <type_traits>

#include "foundation/error.h"
#include "platform/dynamic_library.h"

namespace atlas {

class ClrHost {
 public:
  ClrHost() = default;
  ~ClrHost();

  ClrHost(const ClrHost&) = delete;
  ClrHost& operator=(const ClrHost&) = delete;
  ClrHost(ClrHost&&) noexcept;
  ClrHost& operator=(ClrHost&&) noexcept;

  [[nodiscard]] auto Initialize(const std::filesystem::path& runtime_config_path) -> Result<void>;

  void Finalize();

  // The ClrHost must outlive calls through the returned function pointer.
  [[nodiscard]] auto GetMethod(const std::filesystem::path& assembly_path,
                               std::string_view type_name, std::string_view method_name)
      -> Result<void*>;

  template <typename FuncPtr>
    requires std::is_pointer_v<FuncPtr> && std::is_function_v<std::remove_pointer_t<FuncPtr>>
  [[nodiscard]] auto GetMethodAs(const std::filesystem::path& assembly_path,
                                 std::string_view type_name, std::string_view method_name)
      -> Result<FuncPtr> {
    auto result = GetMethod(assembly_path, type_name, method_name);
    if (!result) return result.Error();
    return reinterpret_cast<FuncPtr>(*result);
  }

  [[nodiscard]] auto IsInitialized() const -> bool { return initialized_; }

 private:
  [[nodiscard]] auto LoadHostfxr() -> Result<void>;

  std::optional<DynamicLibrary> hostfxr_lib_;

  void* host_context_ = nullptr;

  // Stored as void* to keep hostfxr SDK headers out of this public header.
  void* fn_init_config_ = nullptr;
  void* fn_get_delegate_ = nullptr;
  void* fn_close_ = nullptr;

  void* fn_load_assembly_ = nullptr;

  bool initialized_ = false;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_HOST_H_
