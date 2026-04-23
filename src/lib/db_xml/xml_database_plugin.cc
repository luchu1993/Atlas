#include "db/db_export.h"
#include "db_xml/xml_database.h"

ATLAS_DB_BACKEND_API atlas::IDatabase* AtlasCreateDatabase() {
  return new atlas::XmlDatabase();
}
