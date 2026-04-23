#ifndef ATLAS_SERVER_BASEAPP_BASE_ENTITY_H_
#define ATLAS_SERVER_BASEAPP_BASE_ENTITY_H_

#include <cstdint>
#include <vector>

#include "db/idatabase.h"
#include "foundation/clock.h"
#include "network/address.h"
#include "server/entity_types.h"

namespace atlas {

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

class BaseEntity {
 public:
  BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);
  virtual ~BaseEntity() = default;

  // Non-copyable
  BaseEntity(const BaseEntity&) = delete;
  BaseEntity& operator=(const BaseEntity&) = delete;

  [[nodiscard]] auto EntityId() const -> EntityID { return entity_id_; }
  [[nodiscard]] auto TypeId() const -> uint16_t { return type_id_; }
  [[nodiscard]] auto Dbid() const -> DatabaseID { return dbid_; }
  void SetDbid(DatabaseID id) { dbid_ = id; }

  [[nodiscard]] auto HasCell() const -> bool { return cell_entity_id_ != kInvalidEntityID; }
  [[nodiscard]] auto CellEntityId() const -> EntityID { return cell_entity_id_; }
  [[nodiscard]] auto CellAddr() const -> const Address& { return cell_addr_; }
  [[nodiscard]] auto CellEpoch() const -> uint32_t { return cell_epoch_; }
  // Space this entity's cell counterpart lives in (kInvalidSpaceID when
  // base-only). Needed by the CellApp-death restore path to look up a
  // new host in the mgr's rehome table.
  [[nodiscard]] auto SpaceId() const -> SpaceID { return space_id_; }
  void SetSpaceId(SpaceID sid) { space_id_ = sid; }

  // Entity data blob (base-persistent properties serialised by C#
  // Base.Serialize — DATA_BASE scope).
  [[nodiscard]] auto EntityData() const -> const std::vector<std::byte>& { return entity_data_; }
  void SetEntityData(std::vector<std::byte> data) { entity_data_ = std::move(data); }

  // Cell backup blob. Periodically pushed up by the cell (CellApp's
  // BackupCellEntity pump → baseapp handler writes here). The bytes are
  // the output of cell-side C# ServerEntity.Serialize (CELL_DATA subset).
  // BaseApp NEVER deserialises them — it just owns the blob long enough
  // to hand it off to DB writes, reviver, or a migration target.
  [[nodiscard]] auto CellBackupData() const -> const std::vector<std::byte>& {
    return cell_backup_data_;
  }
  void SetCellBackupData(std::vector<std::byte> data) { cell_backup_data_ = std::move(data); }

  // Called by DBApp write-ack
  void OnWriteAck(DatabaseID dbid, bool success);

  // Cell tracking — epoch prevents stale CurrentCell from overwriting a
  // newer placement. CellEntityCreated uses epoch=0 (initial), Offload
  // uses monotonically increasing epochs.
  void SetCell(EntityID cell_eid, const Address& addr, uint32_t epoch = 0);
  void ClearCell();

  // Mark entity as pending destruction
  void MarkForDestroy() { pending_destroy_ = true; }
  [[nodiscard]] auto IsPendingDestroy() const -> bool { return pending_destroy_; }

 protected:
  EntityID entity_id_;
  uint16_t type_id_;
  DatabaseID dbid_;
  EntityID cell_entity_id_{kInvalidEntityID};
  Address cell_addr_;
  uint32_t cell_epoch_{0};
  SpaceID space_id_{kInvalidSpaceID};
  std::vector<std::byte> entity_data_;
  std::vector<std::byte> cell_backup_data_;
  bool pending_destroy_{false};
  bool writing_to_db_{false};
};

// ============================================================================
// Proxy — BaseEntity with an attached client session
//
// Each logged-in client is associated with exactly one Proxy on BaseApp.
// The Proxy routes client RPCs to the entity and relays server RPCs back.
// ============================================================================

class Proxy : public BaseEntity {
 public:
  Proxy(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);

  // Client attachment is tracked by remote address so the live Channel can be
  // resolved on demand instead of being stored in entity state.
  [[nodiscard]] auto ClientAddr() const -> const Address& { return client_addr_; }
  void BindClient(const Address& addr);
  void UnbindClient();

  [[nodiscard]] auto HasClient() const -> bool { return client_attached_; }

  [[nodiscard]] auto GetSessionKey() const -> const ::atlas::SessionKey& { return session_key_; }
  void SetSessionKey(const SessionKey& key) { session_key_ = key; }
  [[nodiscard]] auto SessionEpoch() const -> uint64_t { return session_epoch_; }
  auto BumpSessionEpoch() -> uint64_t { return ++session_epoch_; }

  void EnterDetachedGrace(TimePoint until);
  void ClearDetachedGrace();
  [[nodiscard]] auto IsDetached() const -> bool { return detached_grace_; }
  [[nodiscard]] auto DetachedUntil() const -> TimePoint { return detached_until_; }

 private:
  Address client_addr_;
  bool client_attached_{false};
  SessionKey session_key_;
  uint64_t session_epoch_{0};
  bool detached_grace_{false};
  TimePoint detached_until_{};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASE_ENTITY_H_
