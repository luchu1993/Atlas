#if ATLAS_PLATFORM_WINDOWS

#include "platform/dynamic_library.hpp"

#include <windows.h>

namespace atlas
{

// ============================================================================
// Lifetime
// ============================================================================

DynamicLibrary::~DynamicLibrary()
{
    if (handle_)
    {
        FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other)
    {
        unload();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Loading
// ============================================================================

auto DynamicLibrary::load(const std::filesystem::path& path)
    -> Result<DynamicLibrary>
{
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (!module)
    {
        DWORD err = GetLastError();
        return Error{ErrorCode::IoError,
            "LoadLibrary failed (error " + std::to_string(err) + "): "
            + path.string()};
    }
    return DynamicLibrary{static_cast<void*>(module)};
}

// ============================================================================
// Symbol lookup
// ============================================================================

auto DynamicLibrary::get_symbol_raw(std::string_view name) -> void*
{
    if (!handle_)
    {
        return nullptr;
    }
    std::string name_str(name);
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle_), name_str.c_str()));
}

// ============================================================================
// Unload
// ============================================================================

void DynamicLibrary::unload()
{
    if (handle_)
    {
        FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

} // namespace atlas

#endif // ATLAS_PLATFORM_WINDOWS
