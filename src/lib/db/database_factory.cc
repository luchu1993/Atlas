#include "db/database_factory.h"

#include <vector>

#include "foundation/log.h"
#include "platform/dynamic_library.h"

namespace atlas {

auto CreateDatabase(const DatabaseConfig& config) -> std::unique_ptr<IDatabase> {
  // Build the platform-specific library name from the backend type string.
  // Convention: backend "sqlite" -> library "atlas_db_sqlite(.dll|.so)".
  std::string lib_name = std::format("atlas_db_{}", config.type);

  auto lib_result = DynamicLibrary::Load(lib_name);
  if (!lib_result) {
    ATLAS_LOG_ERROR("CreateDatabase: failed to load backend '{}'", lib_name);
    return nullptr;
  }

  using CreateFn = IDatabase* (*)();
  auto fn_result = lib_result->GetSymbol<CreateFn>("AtlasCreateDatabase");
  if (!fn_result) {
    ATLAS_LOG_ERROR("CreateDatabase: backend '{}' missing AtlasCreateDatabase symbol", lib_name);
    return nullptr;
  }

  // The DynamicLibrary must outlive every IDatabase created from it (vtable
  // pointers live in the DLL's code section).  Park it in a process-lifetime
  // collection — backend DLLs are never unloaded.
  static std::vector<DynamicLibrary> loaded_backends;
  loaded_backends.push_back(std::move(*lib_result));

  return std::unique_ptr<IDatabase>((*fn_result)());
}

}  // namespace atlas
