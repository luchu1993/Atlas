#include "clrscript/clr_object.h"

#include <array>
#include <format>

#include "clrscript/clr_error.h"
#include "clrscript/clr_object_registry.h"
#include "foundation/error.h"
#include "foundation/log.h"

namespace atlas {

// ============================================================================
// GCHandleTracker (Debug only)
// ============================================================================

#if ATLAS_DEBUG
std::atomic<int64_t> GCHandleTracker::alloc_count{0};
std::atomic<int64_t> GCHandleTracker::free_count{0};
#endif

// ============================================================================
// ClrObjectVTable — process-wide singleton
// ============================================================================

static ClrObjectVTable g_vtable{};

void SetClrObjectVtable(const ClrObjectVTable& vtable) {
  ATLAS_ASSERT(vtable.IsValid() && "All ClrObjectVTable function pointers must be non-null");
  g_vtable = vtable;
}

auto GetClrObjectVtable() -> const ClrObjectVTable& {
  return g_vtable;
}

// ============================================================================
// ClrObject implementation
// ============================================================================

ClrObject::ClrObject(void* gc_handle) : gc_handle_(gc_handle) {
  ATLAS_ASSERT(gc_handle != nullptr && "GCHandle must not be null");
#if ATLAS_DEBUG
  GCHandleTracker::OnAlloc();
#endif
  ClrObjectRegistry::Instance().RegisterObject(this);
}

ClrObject::~ClrObject() {
  ClrObjectRegistry::Instance().UnregisterObject(this);
  Release();
}

ClrObject::ClrObject(ClrObject&& other) noexcept : gc_handle_(other.gc_handle_) {
  other.gc_handle_ = nullptr;
  ClrObjectRegistry::Instance().RegisterObject(this);
}

ClrObject& ClrObject::operator=(ClrObject&& other) noexcept {
  if (this != &other) {
    Release();
    gc_handle_ = other.gc_handle_;
    other.gc_handle_ = nullptr;
  }
  return *this;
}

void ClrObject::Release() {
  if (gc_handle_ == nullptr) return;

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (vt.free_handle) {
    vt.free_handle(gc_handle_);
#if ATLAS_DEBUG
    GCHandleTracker::OnFree();
#endif
  } else {
    ATLAS_LOG_WARNING(
        "ClrObject::release() called but vtable.free_handle is null — GCHandle "
        "leak!");
  }

  gc_handle_ = nullptr;
}

// ---- ScriptObject interface -------------------------------------------------

auto ClrObject::IsNone() const -> bool {
  if (gc_handle_ == nullptr) return true;

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.is_none) return true;

  return vt.is_none(gc_handle_) != 0;
}

auto ClrObject::TypeName() const -> std::string {
  if (gc_handle_ == nullptr) return "null";

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.get_type_name) return "unknown";

  std::array<char, 256> buf{};
  const int32_t kLen = vt.get_type_name(gc_handle_, buf.data(), static_cast<int32_t>(buf.size()));
  if (kLen <= 0) return "unknown";

  return std::string(buf.data(), static_cast<std::size_t>(kLen));
}

auto ClrObject::AsInt() const -> Result<int64_t> {
  if (gc_handle_ == nullptr) return Error{ErrorCode::kScriptError, "ClrObject is null"};

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.to_int64)
    return Error{ErrorCode::kScriptError, "ClrObject vtable.to_int64 not registered"};

  int64_t out = 0;
  if (vt.to_int64(gc_handle_, &out) != 0) return ReadClrError();

  return out;
}

auto ClrObject::AsDouble() const -> Result<double> {
  if (gc_handle_ == nullptr) return Error{ErrorCode::kScriptError, "ClrObject is null"};

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.to_double)
    return Error{ErrorCode::kScriptError, "ClrObject vtable.to_double not registered"};

  double out = 0.0;
  if (vt.to_double(gc_handle_, &out) != 0) return ReadClrError();

  return out;
}

auto ClrObject::AsString() const -> Result<std::string> {
  if (gc_handle_ == nullptr) return Error{ErrorCode::kScriptError, "ClrObject is null"};

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.to_string)
    return Error{ErrorCode::kScriptError, "ClrObject vtable.to_string not registered"};

  // Two-pass: first call with nullptr to get required size.
  const int32_t kRequired = vt.to_string(gc_handle_, nullptr, 0);
  if (kRequired < 0) return ReadClrError();
  if (kRequired == 0) return std::string{};

  std::string result(static_cast<std::size_t>(kRequired), '\0');
  const int32_t kWritten = vt.to_string(gc_handle_, result.data(), kRequired);
  if (kWritten < 0) return ReadClrError();

  result.resize(static_cast<std::size_t>(kWritten));
  return result;
}

auto ClrObject::AsBool() const -> Result<bool> {
  if (gc_handle_ == nullptr) return Error{ErrorCode::kScriptError, "ClrObject is null"};

  const ClrObjectVTable& vt = GetClrObjectVtable();
  if (!vt.to_bool) return Error{ErrorCode::kScriptError, "ClrObject vtable.to_bool not registered"};

  uint8_t out = 0;
  if (vt.to_bool(gc_handle_, &out) != 0) return ReadClrError();

  return out != 0;
}

auto ClrObject::AsBytes() const -> Result<std::vector<std::byte>> {
  // Needs byte-array serialization across the managed boundary — not wired yet.
  return Error{ErrorCode::kScriptError, "ClrObject::as_bytes() not implemented"};
}

auto ClrObject::GetAttr(std::string_view /*name*/) -> std::unique_ptr<ScriptObject> {
  // Generic attribute access needs Source Generator scaffolding to be wired
  // up. Return nullptr until then; callers should check for null.
  return nullptr;
}

auto ClrObject::SetAttr(std::string_view /*name*/, const ScriptValue& /*value*/) -> Result<void> {
  return Error{ErrorCode::kScriptError, "ClrObject::set_attr() not implemented"};
}

auto ClrObject::IsCallable() const -> bool {
  // Generic callability needs Source Generator scaffolding to be wired up.
  return false;
}

auto ClrObject::Call(std::span<const ScriptValue> /*args*/) -> Result<ScriptValue> {
  return Error{ErrorCode::kScriptError, "ClrObject::call() not implemented"};
}

auto ClrObject::ToDebugString() const -> std::string {
  if (gc_handle_ == nullptr) return "ClrObject(null)";
  return std::format("ClrObject({})", TypeName());
}

}  // namespace atlas
