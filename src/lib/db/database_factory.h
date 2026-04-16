#ifndef ATLAS_LIB_DB_DATABASE_FACTORY_H_
#define ATLAS_LIB_DB_DATABASE_FACTORY_H_

#include <memory>

#include "db/idatabase.h"

namespace atlas {

/// Create a database backend based on DatabaseConfig::type.
/// Returns nullptr if the type is unknown or the backend is not compiled in.
[[nodiscard]] auto CreateDatabase(const DatabaseConfig& config) -> std::unique_ptr<IDatabase>;

}  // namespace atlas

#endif  // ATLAS_LIB_DB_DATABASE_FACTORY_H_
