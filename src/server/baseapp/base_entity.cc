#include "base_entity.h"

#include "foundation/log.h"

namespace atlas {

BaseEntity::BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid)
    : entity_id_(id), type_id_(type_id), dbid_(dbid) {}

void BaseEntity::OnWriteAck(DatabaseID dbid, bool success) {
  writing_to_db_ = false;
  if (success && dbid != kInvalidDBID) {
    dbid_ = dbid;
  } else if (!success) {
    ATLAS_LOG_WARNING("Entity {} write_to_db failed", entity_id_);
  }
  if (pending_destroy_) {
    // EntityManager scan handles cleanup next tick.
  }
}

void BaseEntity::SetCell(const Address& addr, uint32_t epoch) {
  if (epoch < cell_epoch_) {
    ATLAS_LOG_DEBUG("BaseEntity::SetCell: stale epoch={} (current={}) for entity {} — ignored",
                    epoch, cell_epoch_, entity_id_);
    return;
  }
  cell_addr_ = addr;
  cell_epoch_ = epoch;
}

void BaseEntity::ClearCell() {
  cell_addr_ = {};
  cell_epoch_ = 0;
}

Proxy::Proxy(EntityID id, uint16_t type_id, DatabaseID dbid) : BaseEntity(id, type_id, dbid) {}

void Proxy::BindClient(const Address& addr) {
  client_addr_ = addr;
  client_attached_ = true;
  detached_grace_ = false;
  detached_until_ = {};
}

void Proxy::UnbindClient() {
  client_addr_ = {};
  client_attached_ = false;
}

void Proxy::EnterDetachedGrace(TimePoint until) {
  detached_grace_ = true;
  detached_until_ = until;
}

void Proxy::ClearDetachedGrace() {
  detached_grace_ = false;
  detached_until_ = {};
}

}  // namespace atlas
