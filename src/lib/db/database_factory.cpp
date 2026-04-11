#include "db/database_factory.hpp"

#include "foundation/log.hpp"

// Include backends that are compiled in
#include "db_xml/xml_database.hpp"

#if defined(ATLAS_DB_MYSQL)
#include "db_mysql/mysql_database.hpp"
#endif

namespace atlas
{

auto create_database(const DatabaseConfig& config) -> std::unique_ptr<IDatabase>
{
    if (config.type == "xml")
    {
        return std::make_unique<XmlDatabase>();
    }
#if defined(ATLAS_DB_MYSQL)
    if (config.type == "mysql")
    {
        return std::make_unique<MysqlDatabase>();
    }
#endif

#if defined(ATLAS_DB_MYSQL)
    ATLAS_LOG_ERROR("create_database: unknown backend type '{}' (supported: xml, mysql)",
                    config.type);
#else
    ATLAS_LOG_ERROR("create_database: unknown backend type '{}' (supported: xml)", config.type);
#endif
    return nullptr;
}

}  // namespace atlas
