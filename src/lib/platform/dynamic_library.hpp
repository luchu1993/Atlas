#pragma once

#include "foundation/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// DynamicLibrary
// ============================================================================

class DynamicLibrary
{
public:
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    [[nodiscard]] static auto load(const std::filesystem::path& path)
        -> Result<DynamicLibrary>;

    template <typename FuncPtr>
    [[nodiscard]] auto get_symbol(std::string_view name) -> Result<FuncPtr>
    {
        auto* sym = get_symbol_raw(name);
        if (!sym)
        {
            return Error{ErrorCode::NotFound,
                std::string("Symbol not found: ") + std::string(name)};
        }
        return reinterpret_cast<FuncPtr>(sym);
    }

    [[nodiscard]] auto is_loaded() const -> bool { return handle_ != nullptr; }
    void unload();

private:
    DynamicLibrary() = default;
    explicit DynamicLibrary(void* handle) : handle_(handle) {}
    auto get_symbol_raw(std::string_view name) -> void*;
    void* handle_{nullptr};
};

} // namespace atlas
