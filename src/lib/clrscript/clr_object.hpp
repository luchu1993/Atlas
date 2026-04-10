#pragma once

#include "clrscript/clr_invoke.hpp"
#include "foundation/error.hpp"
#include "script/script_object.hpp"
#include "script/script_value.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// GCHandleTracker — Debug-mode GCHandle leak detector
// ============================================================================
//
// Tracks the cumulative number of GCHandle allocations and frees.
// At process exit (or on demand) leak_count() should return 0.
// Only compiled in ATLAS_DEBUG builds; in Release it compiles away entirely.

#if ATLAS_DEBUG

class GCHandleTracker
{
public:
    static void on_alloc() { alloc_count_.fetch_add(1, std::memory_order_relaxed); }
    static void on_free() { free_count_.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] static auto alloc_count() -> int64_t
    {
        return alloc_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static auto free_count() -> int64_t
    {
        return free_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static auto leak_count() -> int64_t { return alloc_count() - free_count(); }

private:
    static std::atomic<int64_t> alloc_count_;
    static std::atomic<int64_t> free_count_;
};

#endif  // ATLAS_DEBUG

// ============================================================================
// ClrObjectVTable — function pointers injected during CLR bootstrap
// ============================================================================
//
// Rather than going through ClrStaticMethod::bind() per-instance, the engine
// registers a single set of helper function pointers during initialization.
// All ClrObject instances share these pointers.
//
// The C# side (GCHandleHelper) exposes [UnmanagedCallersOnly] methods that
// match these signatures exactly.

struct ClrObjectVTable
{
    // Free a GCHandle.
    // C# signature: [UnmanagedCallersOnly] static void Free(IntPtr handle)
    void (*free_handle)(void* handle){nullptr};

    // Retrieve the CLR type name as UTF-8 into a caller-supplied buffer.
    // Returns the number of bytes written (without null terminator).
    // C# signature: [UnmanagedCallersOnly] static int GetTypeName(IntPtr handle, byte* buf, int
    // len)
    int32_t (*get_type_name)(void* handle, char* buf, int32_t buf_len){nullptr};

    // Test whether the object is null or None.
    // C# signature: [UnmanagedCallersOnly] static byte IsNone(IntPtr handle)  // 0 or 1
    uint8_t (*is_none)(void* handle){nullptr};

    // Convert to int64 (returns 0 on failure, sets CLR error).
    // C# signature: [UnmanagedCallersOnly] static int ToInt64(IntPtr handle, long* out)
    int32_t (*to_int64)(void* handle, int64_t* out){nullptr};

    // Convert to double (returns 0 on failure, sets CLR error).
    // C# signature: [UnmanagedCallersOnly] static int ToDouble(IntPtr handle, double* out)
    int32_t (*to_double)(void* handle, double* out){nullptr};

    // Convert to UTF-8 string (returns byte count on success, -1 on failure).
    // Caller allocates buf; if buf is nullptr / buf_len is 0, returns required size.
    // C# signature: [UnmanagedCallersOnly] static int ToString(IntPtr handle, byte* buf, int len)
    int32_t (*to_string)(void* handle, char* buf, int32_t buf_len){nullptr};

    // Convert to bool (returns 0 or 1 via *out; method return: 0=ok, -1=error).
    // C# signature: [UnmanagedCallersOnly] static int ToBool(IntPtr handle, byte* out)
    int32_t (*to_bool)(void* handle, uint8_t* out){nullptr};

    [[nodiscard]] auto is_valid() const -> bool
    {
        return free_handle && get_type_name && is_none && to_int64 && to_double && to_string &&
               to_bool;
    }
};

// Process-wide vtable.  Must be populated before any ClrObject is constructed.
// Typically called from ClrHost bootstrap after CLR initialization.
void set_clr_object_vtable(const ClrObjectVTable& vtable);
[[nodiscard]] auto get_clr_object_vtable() -> const ClrObjectVTable&;

// ============================================================================
// ClrObject — ScriptObject backed by a .NET GCHandle
// ============================================================================
//
// ClrObject holds a GCHandle (opaque void*) that keeps the managed object
// alive from the GC's perspective.  The handle is freed in the destructor
// by calling the registered free_handle function pointer.
//
// Ownership:
//   ClrObject is the *sole* owner of the GCHandle.  Do not share raw
//   gc_handle() pointers across ClrObject instances.
//
// Thread safety:
//   Individual ClrObject methods are NOT thread-safe.  Do not call methods
//   from multiple threads without external synchronization.

class ClrObject final : public ScriptObject
{
public:
    // Construct with an existing GCHandle obtained from C#.
    // Takes ownership; gc_handle must not be nullptr.
    explicit ClrObject(void* gc_handle);

    ~ClrObject() override;

    // Non-copyable; GCHandle is unique ownership.
    ClrObject(const ClrObject&) = delete;
    ClrObject& operator=(const ClrObject&) = delete;

    // Movable: transfers handle ownership; source becomes empty (is_none() = true).
    ClrObject(ClrObject&& other) noexcept;
    ClrObject& operator=(ClrObject&& other) noexcept;

    // ---- ScriptObject interface ----

    [[nodiscard]] auto is_none() const -> bool override;
    [[nodiscard]] auto type_name() const -> std::string override;

    [[nodiscard]] auto get_attr(std::string_view name) -> std::unique_ptr<ScriptObject> override;
    [[nodiscard]] auto set_attr(std::string_view name, const ScriptValue& value)
        -> Result<void> override;

    [[nodiscard]] auto as_int() const -> Result<int64_t> override;
    [[nodiscard]] auto as_double() const -> Result<double> override;
    [[nodiscard]] auto as_string() const -> Result<std::string> override;
    [[nodiscard]] auto as_bool() const -> Result<bool> override;
    [[nodiscard]] auto as_bytes() const -> Result<std::vector<std::byte>> override;

    [[nodiscard]] auto is_callable() const -> bool override;
    [[nodiscard]] auto call(std::span<const ScriptValue> args = {}) -> Result<ScriptValue> override;

    [[nodiscard]] auto to_debug_string() const -> std::string override;

    // Raw GCHandle access (for passing back to C# helpers).
    [[nodiscard]] auto gc_handle() const -> void* { return gc_handle_; }

private:
    // Free the GCHandle if non-null, then set to null.
    void release();

    void* gc_handle_{nullptr};
};

}  // namespace atlas
