#ifndef ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_

#include <filesystem>
#include <string_view>
#include <type_traits>

#include "clrscript/clr_error.h"
#include "clrscript/clr_host.h"
#include "foundation/error.h"

namespace atlas {

// ============================================================================
// ClrStaticMethod<Ret, Args...> — compile-time-typed C++ → C# call channel
// ============================================================================
//
// Wraps a function pointer to a C# [UnmanagedCallersOnly] static method.
// The type parameters enforce that the caller passes exactly the right argument
// types; mismatches are caught at compile time rather than at runtime.
//
// Lifecycle:
//   1. Declare as a member or local variable (function pointer is null).
//   2. Call bind() once after ClrHost::initialize().
//   3. Call invoke() as many times as needed.
//   4. Call reset() before hot-reload invalidates the pointer.
//
// Error convention for C# entry methods:
//   Each [UnmanagedCallersOnly] method whose return type is int must follow:
//     - Return 0 on success.
//     - Call ErrorBridge.SetError(ex) and return -1 on failure.
//   ClrStaticMethod<int, ...>::invoke() automatically checks this convention
//   and converts a -1 return into a read_clr_error() call.
//
//   Methods returning void or other types bypass this check.  If you need
//   error reporting from a void method, use a separate out-parameter or
//   switch to an int-returning overload.
//
// Thread safety:
//   bind() / reset() must be called from the main thread.
//   invoke() is safe from any thread once bound.
//
// Example:
//   ClrStaticMethod<int, float, float> m_add;
//   m_add.bind(host, asm_path, "MyNamespace.MyClass, MyAssembly", "Add");
//   auto res = m_add.invoke(1.0f, 2.0f);   // Result<int>

template <typename Ret, typename... Args>
class ClrStaticMethod {
  static_assert(!std::is_reference_v<Ret>, "Return type must not be a reference");

 public:
  ClrStaticMethod() = default;

  // Not copyable — a raw function pointer is not a problem, but copying
  // semantics are confusing given the ownership model.
  ClrStaticMethod(const ClrStaticMethod&) = delete;
  ClrStaticMethod& operator=(const ClrStaticMethod&) = delete;

  ClrStaticMethod(ClrStaticMethod&&) noexcept = default;
  ClrStaticMethod& operator=(ClrStaticMethod&&) noexcept = default;

  // -------------------------------------------------------------------------
  // bind() — resolve the C# method pointer from ClrHost
  // -------------------------------------------------------------------------
  //
  //   assembly_path — full path to the managed .dll
  //   type_name     — assembly-qualified type name, e.g.
  //                   "Atlas.Core.Bootstrap, Atlas.Runtime"
  //   method_name   — exact method name (case-sensitive)
  //
  // Returns Error on failure (assembly not found, method not found, etc.).

  [[nodiscard]] auto Bind(ClrHost& host, const std::filesystem::path& assembly_path,
                          std::string_view type_name, std::string_view method_name)
      -> Result<void> {
    auto result = host.GetMethodAs<FnPtr>(assembly_path, type_name, method_name);
    if (!result) return result.Error();
    fn_ = *result;
    return {};
  }

  // -------------------------------------------------------------------------
  // invoke() — call the bound C# method
  // -------------------------------------------------------------------------
  //
  // For Ret = int:
  //   Follows the "return 0 = success, return -1 = error" convention.
  //   A -1 return triggers read_clr_error() which returns an Error.
  //
  // For Ret = void:
  //   Calls the method and returns Result<void>{} unconditionally.
  //   Error detection is the caller's responsibility (e.g. a separate
  //   has_clr_error() check, or an int-returning companion method).
  //
  // For all other Ret types:
  //   Returns the raw return value wrapped in Result<Ret>.

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

  // Clears the cached function pointer.  Must be called before a hot-reload
  // unloads the assembly that the pointer refers to.
  void Reset() { fn_ = nullptr; }

 private:
  using FnPtr = Ret (*)(Args...);
  FnPtr fn_{nullptr};
};

// ============================================================================
// Convenience alias for the most common case: void-returning C# callbacks
// ============================================================================

template <typename... Args>
using ClrVoidMethod = ClrStaticMethod<void, Args...>;

// ============================================================================
// Convenience alias for the error-reporting convention: int-returning methods
// ============================================================================

template <typename... Args>
using ClrFallibleMethod = ClrStaticMethod<int, Args...>;

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_INVOKE_H_
