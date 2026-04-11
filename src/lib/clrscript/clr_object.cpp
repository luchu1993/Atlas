#include "clrscript/clr_object.hpp"

#include "clrscript/clr_error.hpp"
#include "clrscript/clr_object_registry.hpp"
#include "foundation/error.hpp"
#include "foundation/log.hpp"

#include <array>
#include <format>

namespace atlas
{

// ============================================================================
// GCHandleTracker (Debug only)
// ============================================================================

#if ATLAS_DEBUG
std::atomic<int64_t> GCHandleTracker::alloc_count_{0};
std::atomic<int64_t> GCHandleTracker::free_count_{0};
#endif

// ============================================================================
// ClrObjectVTable — process-wide singleton
// ============================================================================

static ClrObjectVTable g_vtable{};

void set_clr_object_vtable(const ClrObjectVTable& vtable)
{
    ATLAS_ASSERT(vtable.is_valid() && "All ClrObjectVTable function pointers must be non-null");
    g_vtable = vtable;
}

auto get_clr_object_vtable() -> const ClrObjectVTable&
{
    return g_vtable;
}

// ============================================================================
// ClrObject implementation
// ============================================================================

ClrObject::ClrObject(void* gc_handle) : gc_handle_(gc_handle)
{
    ATLAS_ASSERT(gc_handle != nullptr && "GCHandle must not be null");
#if ATLAS_DEBUG
    GCHandleTracker::on_alloc();
#endif
    ClrObjectRegistry::instance().register_object(this);
}

ClrObject::~ClrObject()
{
    ClrObjectRegistry::instance().unregister_object(this);
    release();
}

ClrObject::ClrObject(ClrObject&& other) noexcept : gc_handle_(other.gc_handle_)
{
    other.gc_handle_ = nullptr;
    ClrObjectRegistry::instance().register_object(this);
}

ClrObject& ClrObject::operator=(ClrObject&& other) noexcept
{
    if (this != &other)
    {
        release();
        gc_handle_ = other.gc_handle_;
        other.gc_handle_ = nullptr;
    }
    return *this;
}

void ClrObject::release()
{
    if (gc_handle_ == nullptr)
        return;

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (vt.free_handle)
    {
        vt.free_handle(gc_handle_);
#if ATLAS_DEBUG
        GCHandleTracker::on_free();
#endif
    }
    else
    {
        ATLAS_LOG_WARNING(
            "ClrObject::release() called but vtable.free_handle is null — GCHandle "
            "leak!");
    }

    gc_handle_ = nullptr;
}

// ---- ScriptObject interface -------------------------------------------------

auto ClrObject::is_none() const -> bool
{
    if (gc_handle_ == nullptr)
        return true;

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.is_none)
        return true;

    return vt.is_none(gc_handle_) != 0;
}

auto ClrObject::type_name() const -> std::string
{
    if (gc_handle_ == nullptr)
        return "null";

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.get_type_name)
        return "unknown";

    std::array<char, 256> buf{};
    const int32_t len = vt.get_type_name(gc_handle_, buf.data(), static_cast<int32_t>(buf.size()));
    if (len <= 0)
        return "unknown";

    return std::string(buf.data(), static_cast<std::size_t>(len));
}

auto ClrObject::as_int() const -> Result<int64_t>
{
    if (gc_handle_ == nullptr)
        return Error{ErrorCode::ScriptError, "ClrObject is null"};

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.to_int64)
        return Error{ErrorCode::ScriptError, "ClrObject vtable.to_int64 not registered"};

    int64_t out = 0;
    if (vt.to_int64(gc_handle_, &out) != 0)
        return read_clr_error();

    return out;
}

auto ClrObject::as_double() const -> Result<double>
{
    if (gc_handle_ == nullptr)
        return Error{ErrorCode::ScriptError, "ClrObject is null"};

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.to_double)
        return Error{ErrorCode::ScriptError, "ClrObject vtable.to_double not registered"};

    double out = 0.0;
    if (vt.to_double(gc_handle_, &out) != 0)
        return read_clr_error();

    return out;
}

auto ClrObject::as_string() const -> Result<std::string>
{
    if (gc_handle_ == nullptr)
        return Error{ErrorCode::ScriptError, "ClrObject is null"};

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.to_string)
        return Error{ErrorCode::ScriptError, "ClrObject vtable.to_string not registered"};

    // Two-pass: first call with nullptr to get required size.
    const int32_t required = vt.to_string(gc_handle_, nullptr, 0);
    if (required < 0)
        return read_clr_error();
    if (required == 0)
        return std::string{};

    std::string result(static_cast<std::size_t>(required), '\0');
    const int32_t written = vt.to_string(gc_handle_, result.data(), required);
    if (written < 0)
        return read_clr_error();

    result.resize(static_cast<std::size_t>(written));
    return result;
}

auto ClrObject::as_bool() const -> Result<bool>
{
    if (gc_handle_ == nullptr)
        return Error{ErrorCode::ScriptError, "ClrObject is null"};

    const ClrObjectVTable& vt = get_clr_object_vtable();
    if (!vt.to_bool)
        return Error{ErrorCode::ScriptError, "ClrObject vtable.to_bool not registered"};

    uint8_t out = 0;
    if (vt.to_bool(gc_handle_, &out) != 0)
        return read_clr_error();

    return out != 0;
}

auto ClrObject::as_bytes() const -> Result<std::vector<std::byte>>
{
    // Not implemented in Phase 2 — requires byte-array serialization (Phase 3+).
    return Error{ErrorCode::ScriptError, "ClrObject::as_bytes() not implemented in Phase 2"};
}

auto ClrObject::get_attr(std::string_view /*name*/) -> std::unique_ptr<ScriptObject>
{
    // Attribute access requires Source Generator scaffolding (Phase 4).
    // Return nullptr until then; callers should check for null.
    return nullptr;
}

auto ClrObject::set_attr(std::string_view /*name*/, const ScriptValue& /*value*/) -> Result<void>
{
    return Error{ErrorCode::ScriptError, "ClrObject::set_attr() not implemented in Phase 2"};
}

auto ClrObject::is_callable() const -> bool
{
    // Generic callability requires Source Generator scaffolding (Phase 4).
    return false;
}

auto ClrObject::call(std::span<const ScriptValue> /*args*/) -> Result<ScriptValue>
{
    return Error{ErrorCode::ScriptError, "ClrObject::call() not implemented in Phase 2"};
}

auto ClrObject::to_debug_string() const -> std::string
{
    if (gc_handle_ == nullptr)
        return "ClrObject(null)";
    return std::format("ClrObject({})", type_name());
}

}  // namespace atlas
