#pragma once

#include "db/idatabase.hpp"

#include <memory>

namespace atlas
{

/// Create a database backend based on DatabaseConfig::type.
/// Returns nullptr if the type is unknown or the backend is not compiled in.
[[nodiscard]] auto create_database(const DatabaseConfig& config) -> std::unique_ptr<IDatabase>;

}  // namespace atlas
