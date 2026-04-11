#pragma once

#include "db/idatabase.hpp"
#include "network/address.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <vector>

namespace atlas
{

class Channel;

// ============================================================================
// BaseEntity — the "base" portion of a distributed entity living on BaseApp
//
// Responsibilities:
//   • Holds the entity's persistent state blob (loaded from / saved to DB)
//   • Tracks the associated CellEntity address (if any)
//   • Tracks the associated Client channel (Proxy only)
//   • Provides write_to_db() / destroy() lifecycle
// ============================================================================

class BaseEntity
{
public:
    BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);
    virtual ~BaseEntity() = default;

    // Non-copyable
    BaseEntity(const BaseEntity&) = delete;
    BaseEntity& operator=(const BaseEntity&) = delete;

    [[nodiscard]] auto entity_id() const -> EntityID { return entity_id_; }
    [[nodiscard]] auto type_id() const -> uint16_t { return type_id_; }
    [[nodiscard]] auto dbid() const -> DatabaseID { return dbid_; }
    void set_dbid(DatabaseID id) { dbid_ = id; }

    [[nodiscard]] auto has_cell() const -> bool { return cell_entity_id_ != kInvalidEntityID; }
    [[nodiscard]] auto cell_entity_id() const -> EntityID { return cell_entity_id_; }
    [[nodiscard]] auto cell_addr() const -> const Address& { return cell_addr_; }

    // Entity data blob (persistent properties serialised by C#)
    [[nodiscard]] auto entity_data() const -> const std::vector<std::byte>& { return entity_data_; }
    void set_entity_data(std::vector<std::byte> data) { entity_data_ = std::move(data); }

    // Called by DBApp write-ack
    void on_write_ack(DatabaseID dbid, bool success);

    // Cell tracking
    void set_cell(EntityID cell_eid, const Address& addr);
    void clear_cell();

    // Mark entity as pending destruction
    void mark_for_destroy() { pending_destroy_ = true; }
    [[nodiscard]] auto is_pending_destroy() const -> bool { return pending_destroy_; }

protected:
    EntityID entity_id_;
    uint16_t type_id_;
    DatabaseID dbid_;
    EntityID cell_entity_id_{kInvalidEntityID};
    Address cell_addr_;
    std::vector<std::byte> entity_data_;
    bool pending_destroy_{false};
    bool writing_to_db_{false};
};

// ============================================================================
// Proxy — BaseEntity with an attached client Channel
//
// Each logged-in client is associated with exactly one Proxy on BaseApp.
// The Proxy routes client RPCs to the entity and relays server RPCs back.
// ============================================================================

class Proxy : public BaseEntity
{
public:
    Proxy(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);

    // The authenticated network channel to the client (nullptr if disconnected)
    [[nodiscard]] auto client_channel() const -> Channel* { return client_channel_; }
    void set_client_channel(Channel* ch);

    [[nodiscard]] auto has_client() const -> bool { return client_channel_ != nullptr; }

    [[nodiscard]] auto session_key() const -> const SessionKey& { return session_key_; }
    void set_session_key(const SessionKey& key) { session_key_ = key; }

private:
    Channel* client_channel_{nullptr};
    SessionKey session_key_;
};

}  // namespace atlas
