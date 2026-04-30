#ifndef ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_

#include <filesystem>
#include <string_view>
#include <type_traits>

#include "clrscript/clr_error.h"
#include "clrscript/clr_host.h"
#include "foundation/error.h"

namespace atlas {

template <typename Ret, typename... Args>
class ClrStaticMethod {
  static_assert(!std::is_reference_v<Ret>, "Return type must not be a reference");

 public:
  ClrStaticMethod() = default;

  ClrStaticMethod(const ClrStaticMethod&) = delete;
  ClrStaticMethod& operator=(const ClrStaticMethod&) = delete;

  ClrStaticMethod(ClrStaticMethod&&) noexcept = default;
  ClrStaticMethod& operator=(ClrStaticMethod&&) noexcept = default;

  [[nodiscard]] auto Bind(ClrHost& host, const std::filesystem::path& assembly_path,
                          std::string_view type_name, std::string_view method_name)
      -> Result<void> {
    auto result = host.GetMethodAs<FnPtr>(assembly_path, type_name, method_name);
    if (!result) return result.Error();
    fn_ = *result;
    return {};
  }

  [[nodiscard]] auto Invoke(Args... args) const -> Result<Ret> {
    if (!fn_) return Error{ErrorCode::kScriptError, "ClrStaticMethod not bound"};

    if constexpr (std::is_void_v<Ret>) {
      fn_(args...);
      return {};
    } else if constexpr (std::is_same_v<Ret, int>) {
      const int kRet = fn_(args...);
      if (kRet != 0 && HasClrError()) return ReadClrError();
      return kRet;
    } else {
      return fn_(args...);
    }
  }

  [[nodiscard]] auto IsBound() const -> bool { return fn_ != nullptr; }

  // Must be called before hot-reload unloads the assembly.
  void Reset() { fn_ = nullptr; }

 private:
  using FnPtr = Ret (*)(Args...);
  FnPtr fn_{nullptr};
};

template <typename... Args>
using ClrVoidMethod = ClrStaticMethod<void, Args...>;

template <typename... Args>
using ClrFallibleMethod = ClrStaticMethod<int, Args...>;

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_
