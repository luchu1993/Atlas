#include "db/database_factory.h"

#include "foundation/log.h"

// Include backends that are compiled in
#include "db_sqlite/sqlite_database.h"
#include "db_xml/xml_database.h"

#if defined(ATLAS_DB_MYSQL)
#include "db_mysql/mysql_database.h"
#endif

namespace atlas {

auto CreateDatabase(const DatabaseConfig& config) -> std::unique_ptr<IDatabase> {
  if (config.type == "xml") {
    return std::make_unique<XmlDatabase>();
  }
  if (config.type == "sqlite") {
    return std::make_unique<SqliteDatabase>();
  }
#if defined(ATLAS_DB_MYSQL)
  if (config.type == "mysql") {
    return std::make_unique<MysqlDatabase>();
  }
#endif

#if defined(ATLAS_DB_MYSQL)
  ATLAS_LOG_ERROR("create_database: unknown backend type '{}' (supported: xml, sqlite, mysql)",
                  config.type);
#else
  ATLAS_LOG_ERROR("create_database: unknown backend type '{}' (supported: xml, sqlite)",
                  config.type);
#endif
  return nullptr;
}

}  // namespace atlas
