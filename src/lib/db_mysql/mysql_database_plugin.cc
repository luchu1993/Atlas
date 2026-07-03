#include "db/db_export.h"
#include "db_mysql/mysql_database.h"

ATLAS_DB_BACKEND_API atlas::IDatabase* AtlasCreateDatabase() {
  return new atlas::MysqlDatabase();
}
