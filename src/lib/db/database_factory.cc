#include "db/database_factory.h"

#include <vector>

#include "foundation/log.h"
#include "platform/dynamic_library.h"
#include "platform/filesystem.h"
#include "platform/platform_config.h"

namespace atlas {

auto CreateDatabase(const DatabaseConfig& config) -> std::unique_ptr<IDatabase> {
  // dlopen on Linux searches neither the exe directory nor the lib*.so
  // spelling, so resolve the plugin's absolute path next to the executable.
  auto exe_path = fs::ExecutablePath();
  if (!exe_path) {
    ATLAS_LOG_ERROR("CreateDatabase: cannot resolve executable path: {}",
                    exe_path.Error().Message());
    return nullptr;
  }
#if ATLAS_PLATFORM_WINDOWS
  std::string lib_filename = std::format("atlas_db_{}.dll", config.type);
#else
  std::string lib_filename = std::format("libatlas_db_{}.so", config.type);
#endif
  auto lib_path = exe_path->parent_path() / lib_filename;

  auto lib_result = DynamicLibrary::Load(lib_path);
  if (!lib_result) {
    ATLAS_LOG_ERROR("CreateDatabase: failed to load backend '{}': {}", lib_path.string(),
                    lib_result.Error().Message());
    return nullptr;
  }

  using CreateFn = IDatabase* (*)();
  auto fn_result = lib_result->GetSymbol<CreateFn>("AtlasCreateDatabase");
  if (!fn_result) {
    ATLAS_LOG_ERROR("CreateDatabase: backend '{}' missing AtlasCreateDatabase symbol",
                    lib_path.string());
    return nullptr;
  }

  // Vtable pointers live in the plugin DLL, so the library must outlive
  // every IDatabase it produced; park it in a process-lifetime collection.
  static std::vector<DynamicLibrary> loaded_backends;
  loaded_backends.push_back(std::move(*lib_result));

  return std::unique_ptr<IDatabase>((*fn_result)());
}

}  // namespace atlas
