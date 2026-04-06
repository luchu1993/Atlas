#pragma once

#include "foundation/error.hpp"
#include "pyscript/py_object.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// PyInterpreter — manages the Python interpreter lifecycle
// ============================================================================
//
// Call initialize() once at startup, finalize() at shutdown.
// All other methods require the interpreter to be initialized.
//
// Thread safety: initialize() and finalize() must be called from the main
// thread. Other methods require the GIL (use GILGuard if calling from a
// non-Python thread).

class PyInterpreter
{
public:
    struct Config
    {
        std::filesystem::path python_home;         // PYTHONHOME override
        std::vector<std::filesystem::path> paths;  // Additional sys.path entries
        std::string program_name{"atlas"};         // sys.executable name
        bool isolated{true};                       // Isolated interpreter mode
        bool install_signal_handlers{false};       // Install Python signal handlers
    };

    [[nodiscard]] static auto initialize() -> Result<void>;
    [[nodiscard]] static auto initialize(const Config& config) -> Result<void>;
    static void finalize();
    [[nodiscard]] static auto is_initialized() -> bool;

    [[nodiscard]] static auto exec(std::string_view code) -> Result<void>;
    [[nodiscard]] static auto import(std::string_view module_name) -> Result<PyObjectPtr>;
    [[nodiscard]] static auto add_sys_path(const std::filesystem::path& path) -> Result<void>;
    [[nodiscard]] static auto version() -> std::string_view;

    PyInterpreter() = delete;

private:
    static std::atomic<bool> initialized_;
};

}  // namespace atlas
