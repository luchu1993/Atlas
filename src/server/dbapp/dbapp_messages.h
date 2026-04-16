#ifndef ATLAS_SERVER_DBAPP_DBAPP_MESSAGES_H_
#define ATLAS_SERVER_DBAPP_DBAPP_MESSAGES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "db/idatabase.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"

// ============================================================================
// DBApp network messages (IDs 4000–4008)
//
// Direction:
//   BaseApp → DBApp : WriteEntity, CheckoutEntity, CheckinEntity,
//                     DeleteEntity, LookupEntity, AbortCheckout
//   DBApp → BaseApp : WriteEntityAck, CheckoutEntityAck, DeleteEntityAck,
//                     LookupEntityAck, AbortCheckoutAck
// ============================================================================

namespace atlas::dbapp {

// ============================================================================
// LoadMode — how to identify the entity to checkout
// ============================================================================

enum class LoadMode : uint8_t {
  kByDbid = 0,
  kByName = 1,
};

// ============================================================================
// CheckoutStatus — result code for CheckoutEntityAck
// ============================================================================

enum class CheckoutStatus : uint8_t {
  kSuccess = 0,
  kNotFound = 1,
  kAlreadyCheckedOut = 2,
  kDbError = 3,
};

// ============================================================================
// WriteEntity  (BaseApp → DBApp, ID 4000)
// ============================================================================

struct WriteEntity {
  WriteFlags flags{WriteFlags::kNone};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  uint32_t entity_id{0};
  uint32_t request_id{0};
  std::string identifier;
  std::vector<std::byte> blob;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kWriteEntity), "dbapp::WriteEntity",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(flags));
    w.Write(type_id);
    w.Write(dbid);
    w.Write(entity_id);
    w.Write(request_id);
    w.WriteString(identifier);
    w.Write(static_cast<uint32_t>(blob.size()));
    w.WriteBytes(blob);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WriteEntity> {
    auto f = r.Read<uint8_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    auto eid = r.Read<uint32_t>();
    auto rid = r.Read<uint32_t>();
    auto id_str = r.ReadString();
    auto blob_sz = r.Read<uint32_t>();
    if (!f || !ti || !db || !eid || !rid || !id_str || !blob_sz)
      return Error{ErrorCode::kInvalidArgument, "WriteEntity: truncated"};
    auto blob_span = r.ReadBytes(*blob_sz);
    if (!blob_span) return Error{ErrorCode::kInvalidArgument, "WriteEntity: blob truncated"};
    WriteEntity msg;
    msg.flags = static_cast<WriteFlags>(*f);
    msg.type_id = *ti;
    msg.dbid = *db;
    msg.entity_id = *eid;
    msg.request_id = *rid;
    msg.identifier = std::move(*id_str);
    msg.blob.assign(blob_span->begin(), blob_span->end());
    return msg;
  }
};
static_assert(NetworkMessage<WriteEntity>);

// ============================================================================
// WriteEntityAck  (DBApp → BaseApp, ID 4001)
// ============================================================================

struct WriteEntityAck {
  uint32_t request_id{0};
  bool success{false};
  DatabaseID dbid{kInvalidDBID};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kWriteEntityAck),
                                   "dbapp::WriteEntityAck", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(dbid);
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WriteEntityAck> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto db = r.Read<int64_t>();
    auto err = r.ReadString();
    if (!rid || !ok || !db || !err)
      return Error{ErrorCode::kInvalidArgument, "WriteEntityAck: truncated"};
    WriteEntityAck msg;
    msg.request_id = *rid;
    msg.success = *ok != 0;
    msg.dbid = *db;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<WriteEntityAck>);

// ============================================================================
// CheckoutEntity  (BaseApp → DBApp, ID 4002)
// ============================================================================

struct CheckoutEntity {
  LoadMode mode{LoadMode::kByDbid};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  std::string identifier;
  uint32_t entity_id{0};
  uint32_t request_id{0};
  Address owner_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kCheckoutEntity),
                                   "dbapp::CheckoutEntity", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(mode));
    w.Write(type_id);
    w.Write(dbid);
    w.WriteString(identifier);
    w.Write(entity_id);
    w.Write(request_id);
    w.Write(owner_addr.Ip());
    w.Write(owner_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<CheckoutEntity> {
    auto m = r.Read<uint8_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    auto id_str = r.ReadString();
    auto eid = r.Read<uint32_t>();
    auto rid = r.Read<uint32_t>();
    auto oip = r.Read<uint32_t>();
    auto oport = r.Read<uint16_t>();
    if (!m || !ti || !db || !id_str || !eid || !rid || !oip || !oport)
      return Error{ErrorCode::kInvalidArgument, "CheckoutEntity: truncated"};
    CheckoutEntity msg;
    msg.mode = static_cast<LoadMode>(*m);
    msg.type_id = *ti;
    msg.dbid = *db;
    msg.identifier = std::move(*id_str);
    msg.entity_id = *eid;
    msg.request_id = *rid;
    msg.owner_addr = Address(*oip, *oport);
    return msg;
  }
};
static_assert(NetworkMessage<CheckoutEntity>);

// ============================================================================
// CheckoutEntityAck  (DBApp → BaseApp, ID 4003)
// ============================================================================

struct CheckoutEntityAck {
  uint32_t request_id{0};
  CheckoutStatus status{CheckoutStatus::kSuccess};
  DatabaseID dbid{kInvalidDBID};
  std::vector<std::byte> blob;
  Address holder_addr;
  uint32_t holder_app_id{0};
  uint32_t holder_entity_id{0};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kCheckoutEntityAck),
                                   "dbapp::CheckoutEntityAck", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(status));
    w.Write(dbid);
    w.Write(static_cast<uint32_t>(blob.size()));
    w.WriteBytes(blob);
    w.Write(holder_addr.Ip());
    w.Write(holder_addr.Port());
    w.Write(holder_app_id);
    w.Write(holder_entity_id);
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CheckoutEntityAck> {
    auto rid = r.Read<uint32_t>();
    auto st = r.Read<uint8_t>();
    auto db = r.Read<int64_t>();
    auto blob_sz = r.Read<uint32_t>();
    if (!rid || !st || !db || !blob_sz)
      return Error{ErrorCode::kInvalidArgument, "CheckoutEntityAck: truncated"};
    auto blob_span = r.ReadBytes(*blob_sz);
    auto h_ip = r.Read<uint32_t>();
    auto h_port = r.Read<uint16_t>();
    auto h_app = r.Read<uint32_t>();
    auto h_eid = r.Read<uint32_t>();
    auto err = r.ReadString();
    if (!blob_span || !h_ip || !h_port || !h_app || !h_eid || !err)
      return Error{ErrorCode::kInvalidArgument, "CheckoutEntityAck: field truncated"};
    CheckoutEntityAck msg;
    msg.request_id = *rid;
    msg.status = static_cast<CheckoutStatus>(*st);
    msg.dbid = *db;
    msg.blob.assign(blob_span->begin(), blob_span->end());
    msg.holder_addr = Address(*h_ip, *h_port);
    msg.holder_app_id = *h_app;
    msg.holder_entity_id = *h_eid;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<CheckoutEntityAck>);

// ============================================================================
// CheckinEntity  (BaseApp → DBApp, ID 4004)
// ============================================================================

struct CheckinEntity {
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kCheckinEntity),
                                   "dbapp::CheckinEntity", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint16_t) + sizeof(int64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(type_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CheckinEntity> {
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!ti || !db) return Error{ErrorCode::kInvalidArgument, "CheckinEntity: truncated"};
    CheckinEntity msg;
    msg.type_id = *ti;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<CheckinEntity>);

// ============================================================================
// DeleteEntity  (BaseApp → DBApp, ID 4005)
// ============================================================================

struct DeleteEntity {
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  uint32_t request_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::DBApp::kDeleteEntity), "dbapp::DeleteEntity", MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint16_t) + sizeof(int64_t) + sizeof(uint32_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(type_id);
    w.Write(dbid);
    w.Write(request_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<DeleteEntity> {
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    auto rid = r.Read<uint32_t>();
    if (!ti || !db || !rid) return Error{ErrorCode::kInvalidArgument, "DeleteEntity: truncated"};
    DeleteEntity msg;
    msg.type_id = *ti;
    msg.dbid = *db;
    msg.request_id = *rid;
    return msg;
  }
};
static_assert(NetworkMessage<DeleteEntity>);

// ============================================================================
// DeleteEntityAck  (DBApp → BaseApp, ID 4006)
// ============================================================================

struct DeleteEntityAck {
  uint32_t request_id{0};
  bool success{false};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kDeleteEntityAck),
                                   "dbapp::DeleteEntityAck", MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<DeleteEntityAck> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto err = r.ReadString();
    if (!rid || !ok || !err)
      return Error{ErrorCode::kInvalidArgument, "DeleteEntityAck: truncated"};
    DeleteEntityAck msg;
    msg.request_id = *rid;
    msg.success = *ok != 0;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<DeleteEntityAck>);

// ============================================================================
// LookupEntity  (BaseApp → DBApp, ID 4007)
// ============================================================================

struct LookupEntity {
  uint16_t type_id{0};
  std::string identifier;
  uint32_t request_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kLookupEntity), "dbapp::LookupEntity",
                                   MessageLengthStyle::kVariable, -1};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(type_id);
    w.WriteString(identifier);
    w.Write(request_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<LookupEntity> {
    auto ti = r.Read<uint16_t>();
    auto id_str = r.ReadString();
    auto rid = r.Read<uint32_t>();
    if (!ti || !id_str || !rid)
      return Error{ErrorCode::kInvalidArgument, "LookupEntity: truncated"};
    LookupEntity msg;
    msg.type_id = *ti;
    msg.identifier = std::move(*id_str);
    msg.request_id = *rid;
    return msg;
  }
};
static_assert(NetworkMessage<LookupEntity>);

// ============================================================================
// LookupEntityAck  (DBApp → BaseApp, ID 4008)
// ============================================================================

struct LookupEntityAck {
  uint32_t request_id{0};
  bool found{false};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kLookupEntityAck),
                                   "dbapp::LookupEntityAck", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + 1 + sizeof(int64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(found ? 1 : 0));
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<LookupEntityAck> {
    auto rid = r.Read<uint32_t>();
    auto f = r.Read<uint8_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !f || !db) return Error{ErrorCode::kInvalidArgument, "LookupEntityAck: truncated"};
    LookupEntityAck msg;
    msg.request_id = *rid;
    msg.found = *f != 0;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<LookupEntityAck>);

// ============================================================================
// AbortCheckout  (BaseApp → DBApp, ID 4009)
// Cancel a previously issued CheckoutEntity request that is no longer needed.
// ============================================================================

struct AbortCheckout {
  uint32_t request_id{0};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::DBApp::kAbortCheckout), "dbapp::AbortCheckout",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int64_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(type_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AbortCheckout> {
    auto rid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !ti || !db) return Error{ErrorCode::kInvalidArgument, "AbortCheckout: truncated"};
    AbortCheckout msg;
    msg.request_id = *rid;
    msg.type_id = *ti;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<AbortCheckout>);

// ============================================================================
// AbortCheckoutAck  (DBApp → BaseApp, ID 4010)
// ============================================================================

struct AbortCheckoutAck {
  uint32_t request_id{0};
  bool success{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kAbortCheckoutAck),
                                   "dbapp::AbortCheckoutAck", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<AbortCheckoutAck> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    if (!rid || !ok) return Error{ErrorCode::kInvalidArgument, "AbortCheckoutAck: truncated"};
    AbortCheckoutAck msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    return msg;
  }
};
static_assert(NetworkMessage<AbortCheckoutAck>);

// ============================================================================
// GetEntityIds  (BaseApp → DBApp, ID 4020)
// Request a batch of EntityIDs from the authoritative allocator.
// ============================================================================

struct GetEntityIds {
  uint32_t count{0};  // number of IDs requested

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kGetEntityIds), "dbapp::GetEntityIds",
                                   MessageLengthStyle::kFixed, static_cast<int>(sizeof(uint32_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(count); }

  static auto Deserialize(BinaryReader& r) -> Result<GetEntityIds> {
    auto c = r.Read<uint32_t>();
    if (!c) return Error{ErrorCode::kInvalidArgument, "GetEntityIds: truncated"};
    GetEntityIds msg;
    msg.count = *c;
    return msg;
  }
};
static_assert(NetworkMessage<GetEntityIds>);

// ============================================================================
// GetEntityIdsAck  (DBApp → BaseApp, ID 4021)
// Returns a contiguous range [start, start + count - 1].
// ============================================================================

struct GetEntityIdsAck {
  EntityID start{kInvalidEntityID};
  EntityID end{kInvalidEntityID};
  uint32_t count{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kGetEntityIdsAck),
                                   "dbapp::GetEntityIdsAck", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID) * 2 + sizeof(uint32_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(start);
    w.Write(end);
    w.Write(count);
  }

  static auto Deserialize(BinaryReader& r) -> Result<GetEntityIdsAck> {
    auto s = r.Read<EntityID>();
    auto e = r.Read<EntityID>();
    auto c = r.Read<uint32_t>();
    if (!s || !e || !c) return Error{ErrorCode::kInvalidArgument, "GetEntityIdsAck: truncated"};
    GetEntityIdsAck msg;
    msg.start = *s;
    msg.end = *e;
    msg.count = *c;
    return msg;
  }
};
static_assert(NetworkMessage<GetEntityIdsAck>);

// ============================================================================
// PutEntityIds  (BaseApp → DBApp, ID 4022)
// Return unused EntityIDs (currently a no-op on DBApp side).
// ============================================================================

struct PutEntityIds {
  EntityID start{kInvalidEntityID};
  EntityID end{kInvalidEntityID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kPutEntityIds), "dbapp::PutEntityIds",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(EntityID) * 2)};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(start);
    w.Write(end);
  }

  static auto Deserialize(BinaryReader& r) -> Result<PutEntityIds> {
    auto s = r.Read<EntityID>();
    auto e = r.Read<EntityID>();
    if (!s || !e) return Error{ErrorCode::kInvalidArgument, "PutEntityIds: truncated"};
    PutEntityIds msg;
    msg.start = *s;
    msg.end = *e;
    return msg;
  }
};
static_assert(NetworkMessage<PutEntityIds>);

// ============================================================================
// PutEntityIdsAck  (DBApp → BaseApp, ID 4023)
// ============================================================================

struct PutEntityIdsAck {
  bool success{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kPutEntityIdsAck),
                                   "dbapp::PutEntityIdsAck", MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint8_t))};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(static_cast<uint8_t>(success ? 1 : 0)); }

  static auto Deserialize(BinaryReader& r) -> Result<PutEntityIdsAck> {
    auto ok = r.Read<uint8_t>();
    if (!ok) return Error{ErrorCode::kInvalidArgument, "PutEntityIdsAck: truncated"};
    PutEntityIdsAck msg;
    msg.success = (*ok != 0);
    return msg;
  }
};
static_assert(NetworkMessage<PutEntityIdsAck>);

}  // namespace atlas::dbapp

// ============================================================================
// Authentication messages delegated to DBApp (IDs 5002–5003 reused here)
// These are the same message types defined in login_messages.hpp, but
// re-exported in this header so DBApp only needs to include one file.
// DBApp registers handlers using the same atlas::login:: types.
//
// To avoid ODR issues, do NOT include both headers in the same translation
// unit.  In practice, DBApp includes only this header for auth messages.
// ============================================================================

// AuthLogin / AuthLoginResult are defined in loginapp/login_messages.hpp.
// DBApp includes that header directly when handling auth messages.

#endif  // ATLAS_SERVER_DBAPP_DBAPP_MESSAGES_H_
