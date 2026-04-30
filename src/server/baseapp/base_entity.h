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

// "Base" portion of a distributed entity living on BaseApp; owns persistent
// state and tracks its CellEntity placement.
class BaseEntity {
 public:
  BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);
  virtual ~BaseEntity() = default;

  BaseEntity(const BaseEntity&) = delete;
  BaseEntity& operator=(const BaseEntity&) = delete;

  [[nodiscard]] auto EntityId() const -> EntityID { return entity_id_; }
  [[nodiscard]] auto TypeId() const -> uint16_t { return type_id_; }
  [[nodiscard]] auto Dbid() const -> DatabaseID { return dbid_; }
  void SetDbid(DatabaseID id) { dbid_ = id; }

  [[nodiscard]] auto HasCell() const -> bool { return cell_addr_.Ip() != 0; }
  [[nodiscard]] auto CellAddr() const -> const Address& { return cell_addr_; }
  [[nodiscard]] auto CellEpoch() const -> uint32_t { return cell_epoch_; }
  // kInvalidSpaceID when base-only; used by CellApp-death restore to find
  // a new host in the mgr's rehome table.
  [[nodiscard]] auto SpaceId() const -> SpaceID { return space_id_; }
  void SetSpaceId(SpaceID sid) { space_id_ = sid; }

  // DATA_BASE-scope blob from C# Base.Serialize.
  [[nodiscard]] auto EntityData() const -> const std::vector<std::byte>& { return entity_data_; }
  void SetEntityData(std::vector<std::byte> data) { entity_data_ = std::move(data); }

  // CELL_DATA-scope blob pushed up by the cell. BaseApp NEVER deserialises
  // it — just holds it for DB writes, reviver, or migration.
  [[nodiscard]] auto CellBackupData() const -> const std::vector<std::byte>& {
    return cell_backup_data_;
  }
  void SetCellBackupData(std::vector<std::byte> data) { cell_backup_data_ = std::move(data); }

  void OnWriteAck(DatabaseID dbid, bool success);

  // Epoch prevents stale CurrentCell from overwriting newer placement;
  // initial create uses 0, Offload uses monotonically increasing values.
  void SetCell(const Address& addr, uint32_t epoch = 0);
  void ClearCell();

  void MarkForDestroy() { pending_destroy_ = true; }
  [[nodiscard]] auto IsPendingDestroy() const -> bool { return pending_destroy_; }

 protected:
  EntityID entity_id_;
  uint16_t type_id_;
  DatabaseID dbid_;
  Address cell_addr_;
  uint32_t cell_epoch_{0};
  SpaceID space_id_{kInvalidSpaceID};
  std::vector<std::byte> entity_data_;
  std::vector<std::byte> cell_backup_data_;
  bool pending_destroy_{false};
  bool writing_to_db_{false};
};

// BaseEntity with an attached client session; routes client<->server RPCs.
class Proxy : public BaseEntity {
 public:
  Proxy(EntityID id, uint16_t type_id, DatabaseID dbid = kInvalidDBID);

  // Tracked by remote address so the live Channel resolves on demand.
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
