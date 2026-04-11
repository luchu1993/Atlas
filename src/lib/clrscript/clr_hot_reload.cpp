#include "clrscript/clr_hot_reload.hpp"

#include "clrscript/clr_object_registry.hpp"
#include "clrscript/clr_script_engine.hpp"
#include "entitydef/entity_def_registry.hpp"
#include "foundation/log.hpp"

#include <cstdlib>
#include <format>

namespace atlas
{

ClrHotReload::ClrHotReload(ClrScriptEngine& engine) : engine_(engine) {}

auto ClrHotReload::configure(const Config& config) -> Result<void>
{
    config_ = config;
    if (config_.enabled && std::filesystem::exists(config_.script_project_path))
    {
        auto watch_dir = std::filesystem::is_directory(config_.script_project_path)
                             ? config_.script_project_path
                             : config_.script_project_path.parent_path();
        watcher_.emplace(watch_dir);
        ATLAS_LOG_INFO("ClrHotReload: watching {} for changes", watch_dir.string());
    }
    return {};
}

auto ClrHotReload::reload() -> Result<void>
{
    return do_reload();
}

void ClrHotReload::poll()
{
    if (!config_.enabled || !watcher_)
        return;

    auto now = Clock::now();

    if (debouncing_)
    {
        if (now - last_change_time_ >= config_.debounce_delay)
        {
            debouncing_ = false;
            pending_reload_.store(true, std::memory_order_relaxed);
        }
        return;
    }

    if (watcher_->check_changes())
    {
        ATLAS_LOG_INFO("ClrHotReload: file changes detected, debouncing...");
        last_change_time_ = now;
        debouncing_ = true;
    }
}

auto ClrHotReload::process_pending() -> Result<void>
{
    if (!pending_reload_.exchange(false, std::memory_order_relaxed))
        return {};

    ATLAS_LOG_INFO("ClrHotReload: processing pending reload");
    return do_reload();
}

auto ClrHotReload::compile_scripts() -> Result<void>
{
    auto temp_dir = config_.output_directory / ".reload_staging";
    std::filesystem::create_directories(temp_dir);

    auto cmd = std::format("dotnet build \"{}\" -c Debug -o \"{}\" --nologo -v q",
                           config_.script_project_path.string(), temp_dir.string());

    ATLAS_LOG_INFO("ClrHotReload: compiling scripts...");
    int exit_code = std::system(cmd.c_str());
    if (exit_code != 0)
    {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
        return Error{ErrorCode::ScriptError,
                     std::format("dotnet build failed with exit code {}", exit_code)};
    }

    // Backup current DLLs
    auto backup_dir = config_.output_directory / ".reload_backup";
    std::filesystem::create_directories(backup_dir);

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(config_.output_directory, ec))
    {
        if (entry.path().extension() == ".dll" || entry.path().extension() == ".pdb")
        {
            auto dest = backup_dir / entry.path().filename();
            std::filesystem::copy(entry.path(), dest,
                                  std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    // Move compiled output to the real directory
    for (const auto& entry : std::filesystem::directory_iterator(temp_dir, ec))
    {
        auto dest = config_.output_directory / entry.path().filename();
        std::filesystem::copy(entry.path(), dest, std::filesystem::copy_options::overwrite_existing,
                              ec);
    }
    std::filesystem::remove_all(temp_dir, ec);

    ATLAS_LOG_INFO("ClrHotReload: compilation succeeded");
    return {};
}

auto ClrHotReload::do_reload() -> Result<void>
{
    ATLAS_LOG_INFO("Hot reload: starting...");

    // 1. Compile if enabled
    if (config_.auto_compile)
    {
        auto compile_result = compile_scripts();
        if (!compile_result)
        {
            ATLAS_LOG_ERROR("Hot reload: compile failed: {}", compile_result.error().message());
            return compile_result.error();
        }
    }

    // 2. C# side: serialize entity state and unload script context
    auto serialize_result = engine_.call_hot_reload("SerializeAndUnload");
    if (!serialize_result)
    {
        ATLAS_LOG_ERROR("Hot reload: SerializeAndUnload failed: {}",
                        serialize_result.error().message());
        return serialize_result.error();
    }

    // Release all C++ ClrObject instances holding GCHandles to script-assembly objects
    ClrObjectRegistry::instance().release_all();

    // 3. Clear C++ entity type registry (C# will re-register after load)
    EntityDefRegistry::instance().clear();

    // 4. C# side: load new assembly and restore entity state
    auto assembly_path = config_.output_directory / "Atlas.GameScripts.dll";
    auto load_result = engine_.call_hot_reload("LoadAndRestore", assembly_path);

    if (!load_result)
    {
        ATLAS_LOG_ERROR("Hot reload: LoadAndRestore failed: {}", load_result.error().message());

        // Attempt rollback from backup
        auto backup_dir = config_.output_directory / ".reload_backup";
        std::error_code ec;
        if (std::filesystem::exists(backup_dir, ec) && !std::filesystem::is_empty(backup_dir, ec))
        {
            ATLAS_LOG_WARNING("Hot reload: attempting rollback from backup");
            for (const auto& entry : std::filesystem::directory_iterator(backup_dir, ec))
            {
                auto dest = config_.output_directory / entry.path().filename();
                std::filesystem::copy(entry.path(), dest,
                                      std::filesystem::copy_options::overwrite_existing, ec);
            }
            auto rollback = engine_.call_hot_reload("LoadAndRestore", assembly_path);
            if (!rollback)
            {
                ATLAS_LOG_CRITICAL("Hot reload: rollback also failed: {}",
                                   rollback.error().message());
            }
            else
            {
                ATLAS_LOG_INFO("Hot reload: rollback succeeded");
            }
        }
        else
        {
            ATLAS_LOG_CRITICAL(
                "Hot reload: no backup available for rollback. "
                "Server will continue without script assembly loaded.");
        }
        return load_result.error();
    }

    // 5. Reset file watcher to avoid re-triggering
    if (watcher_)
        watcher_->reset();

    ATLAS_LOG_INFO("Hot reload: complete");
    return {};
}

}  // namespace atlas
