#include "clrscript/clr_hot_reload.h"

#include <cstdlib>
#include <format>

#include "clrscript/clr_object_registry.h"
#include "clrscript/clr_script_engine.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"

namespace atlas {

ClrHotReload::ClrHotReload(ClrScriptEngine& engine) : engine_(engine) {}

auto ClrHotReload::Configure(const Config& config) -> Result<void> {
  config_ = config;
  if (config_.enabled && std::filesystem::exists(config_.script_project_path)) {
    auto watch_dir = std::filesystem::is_directory(config_.script_project_path)
                         ? config_.script_project_path
                         : config_.script_project_path.parent_path();
    watcher_.emplace(watch_dir);
    ATLAS_LOG_INFO("ClrHotReload: watching {} for changes", watch_dir.string());
  }
  return {};
}

auto ClrHotReload::Reload() -> Result<void> {
  return DoReload();
}

void ClrHotReload::Poll() {
  if (!config_.enabled || !watcher_) return;

  auto now = Clock::now();

  if (debouncing_) {
    if (now - last_change_time_ >= config_.debounce_delay) {
      debouncing_ = false;
      pending_reload_.store(true, std::memory_order_relaxed);
    }
    return;
  }

  if (watcher_->CheckChanges()) {
    ATLAS_LOG_INFO("ClrHotReload: file changes detected, debouncing...");
    last_change_time_ = now;
    debouncing_ = true;
  }
}

auto ClrHotReload::ProcessPending() -> Result<void> {
  if (!pending_reload_.exchange(false, std::memory_order_relaxed)) return {};

  ATLAS_LOG_INFO("ClrHotReload: processing pending reload");
  return DoReload();
}

auto ClrHotReload::CompileScripts() const -> Result<void> {
  auto temp_dir = config_.output_directory / ".reload_staging";
  std::filesystem::create_directories(temp_dir);

  auto cmd = std::format("dotnet build \"{}\" -c Debug -o \"{}\" --nologo -v q",
                         config_.script_project_path.string(), temp_dir.string());

  ATLAS_LOG_INFO("ClrHotReload: compiling scripts...");
  int exit_code = std::system(cmd.c_str());
  if (exit_code != 0) {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return Error{ErrorCode::kScriptError,
                 std::format("dotnet build failed with exit code {}", exit_code)};
  }

  auto backup_dir = config_.output_directory / ".reload_backup";
  std::filesystem::create_directories(backup_dir);

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(config_.output_directory, ec)) {
    if (entry.path().extension() == ".dll" || entry.path().extension() == ".pdb") {
      auto dest = backup_dir / entry.path().filename();
      std::filesystem::copy(entry.path(), dest, std::filesystem::copy_options::overwrite_existing,
                            ec);
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(temp_dir, ec)) {
    auto dest = config_.output_directory / entry.path().filename();
    std::filesystem::copy(entry.path(), dest, std::filesystem::copy_options::overwrite_existing,
                          ec);
  }
  std::filesystem::remove_all(temp_dir, ec);

  ATLAS_LOG_INFO("ClrHotReload: compilation succeeded");
  return {};
}

auto ClrHotReload::DoReload() -> Result<void> {
  ATLAS_LOG_INFO("Hot reload: starting...");

  if (config_.auto_compile) {
    auto compile_result = CompileScripts();
    if (!compile_result) {
      ATLAS_LOG_ERROR("Hot reload: compile failed: {}", compile_result.Error().Message());
      return compile_result.Error();
    }
  }

  auto serialize_result = engine_.CallHotReload("SerializeAndUnload");
  if (!serialize_result) {
    ATLAS_LOG_ERROR("Hot reload: SerializeAndUnload failed: {}",
                    serialize_result.Error().Message());
    return serialize_result.Error();
  }

  ClrObjectRegistry::Instance().ReleaseAll();

  EntityDefRegistry::Instance().clear();

  auto assembly_path = config_.output_directory / config_.assembly_name;
  auto load_result = engine_.CallHotReload("LoadAndRestore", assembly_path);

  if (!load_result) {
    ATLAS_LOG_ERROR("Hot reload: LoadAndRestore failed: {}", load_result.Error().Message());

    auto backup_dir = config_.output_directory / ".reload_backup";
    std::error_code ec;
    if (std::filesystem::exists(backup_dir, ec) && !std::filesystem::is_empty(backup_dir, ec)) {
      ATLAS_LOG_WARNING("Hot reload: attempting rollback from backup");
      for (const auto& entry : std::filesystem::directory_iterator(backup_dir, ec)) {
        auto dest = config_.output_directory / entry.path().filename();
        std::filesystem::copy(entry.path(), dest, std::filesystem::copy_options::overwrite_existing,
                              ec);
      }
      auto rollback = engine_.CallHotReload("LoadAndRestore", assembly_path);
      if (!rollback) {
        ATLAS_LOG_CRITICAL("Hot reload: rollback also failed: {}", rollback.Error().Message());
      } else {
        ATLAS_LOG_INFO("Hot reload: rollback succeeded");
      }
    } else {
      ATLAS_LOG_CRITICAL(
          "Hot reload: no backup available for rollback. "
          "Server will continue without script assembly loaded.");
    }
    return load_result.Error();
  }

  if (watcher_) watcher_->Reset();

  ATLAS_LOG_INFO("Hot reload: complete");
  return {};
}

}  // namespace atlas
