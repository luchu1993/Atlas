#ifndef ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "clrscript/clr_invoke.h"
#include "foundation/error.h"
#include "script/script_object.h"
#include "script/script_value.h"

namespace atlas {

class ClrObjectRegistry;

#if ATLAS_DEBUG

class GCHandleTracker {
 public:
  static void OnAlloc() { alloc_count.fetch_add(1, std::memory_order_relaxed); }
  static void OnFree() { free_count.fetch_add(1, std::memory_order_relaxed); }

  [[nodiscard]] static auto AllocCount() -> int64_t {
    return alloc_count.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto FreeCount() -> int64_t {
    return free_count.load(std::memory_order_relaxed);
  }
  [[nodiscard]] static auto LeakCount() -> int64_t { return AllocCount() - FreeCount(); }

 private:
  static std::atomic<int64_t> alloc_count;
  static std::atomic<int64_t> free_count;
};

#endif  // ATLAS_DEBUG

// Function pointers injected during CLR bootstrap.
struct ClrObjectVTable {
  void (*free_handle)(void* handle){nullptr};

  int32_t (*get_type_name)(void* handle, char* buf, int32_t buf_len){nullptr};

  uint8_t (*is_none)(void* handle){nullptr};

  int32_t (*to_int64)(void* handle, int64_t* out){nullptr};

  int32_t (*to_double)(void* handle, double* out){nullptr};

  // If buf is nullptr or empty, returns required UTF-8 byte count.
  int32_t (*to_string)(void* handle, char* buf, int32_t buf_len){nullptr};

  int32_t (*to_bool)(void* handle, uint8_t* out){nullptr};

  [[nodiscard]] auto IsValid() const -> bool {
    return free_handle && get_type_name && is_none && to_int64 && to_double && to_string && to_bool;
  }
};

// Must be populated before any ClrObject is constructed.
void SetClrObjectVtable(const ClrObjectVTable& vtable);
[[nodiscard]] auto GetClrObjectVtable() -> const ClrObjectVTable&;

class ClrObject final : public ScriptObject {
 public:
  // Takes ownership of a non-null GCHandle obtained from C#.
  explicit ClrObject(void* gc_handle);

  ~ClrObject() override;

  ClrObject(const ClrObject&) = delete;
  ClrObject& operator=(const ClrObject&) = delete;

  ClrObject(ClrObject&& other) noexcept;
  ClrObject& operator=(ClrObject&& other) noexcept;

  [[nodiscard]] auto IsNone() const -> bool override;
  [[nodiscard]] auto TypeName() const -> std::string override;

  [[nodiscard]] auto GetAttr(std::string_view name) -> std::unique_ptr<ScriptObject> override;
  [[nodiscard]] auto SetAttr(std::string_view name, const ScriptValue& value)
      -> Result<void> override;

  [[nodiscard]] auto AsInt() const -> Result<int64_t> override;
  [[nodiscard]] auto AsDouble() const -> Result<double> override;
  [[nodiscard]] auto AsString() const -> Result<std::string> override;
  [[nodiscard]] auto AsBool() const -> Result<bool> override;
  [[nodiscard]] auto AsBytes() const -> Result<std::vector<std::byte>> override;

  [[nodiscard]] auto IsCallable() const -> bool override;
  [[nodiscard]] auto Call(std::span<const ScriptValue> args) -> Result<ScriptValue> override;

  [[nodiscard]] auto ToDebugString() const -> std::string override;

  [[nodiscard]] auto GcHandle() const -> void* { return gc_handle_; }

 private:
  friend class ClrObjectRegistry;

  // Free the GCHandle if non-null, then set to null.
  void Release();

  void* gc_handle_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_OBJECT_H_
