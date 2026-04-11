#include "base_entity.hpp"

#include "foundation/log.hpp"

namespace atlas
{

BaseEntity::BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid)
    : entity_id_(id), type_id_(type_id), dbid_(dbid)
{
}

void BaseEntity::on_write_ack(DatabaseID dbid, bool success)
{
    writing_to_db_ = false;
    if (success && dbid != kInvalidDBID)
    {
        dbid_ = dbid;
    }
    else if (!success)
    {
        ATLAS_LOG_WARNING("Entity {} write_to_db failed", entity_id_);
    }
    if (pending_destroy_)
    {
        // Will be cleaned up by EntityManager on next tick scan
    }
}

void BaseEntity::set_cell(EntityID cell_eid, const Address& addr)
{
    cell_entity_id_ = cell_eid;
    cell_addr_ = addr;
}

void BaseEntity::clear_cell()
{
    cell_entity_id_ = kInvalidEntityID;
    cell_addr_ = {};
}

// ============================================================================
// Proxy
// ============================================================================

Proxy::Proxy(EntityID id, uint16_t type_id, DatabaseID dbid) : BaseEntity(id, type_id, dbid) {}

void Proxy::set_client_channel(Channel* ch)
{
    client_channel_ = ch;
}

}  // namespace atlas
