#ifndef ATLAS_SERVER_DBAPPMGR_DBAPPMGR_MESSAGES_H_
#define ATLAS_SERVER_DBAPPMGR_DBAPPMGR_MESSAGES_H_

#include <cstdint>
#include <vector>

#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

namespace atlas::dbappmgr {

struct ShardEntry {
  DatabaseID low_dbid{1};
  DatabaseID high_dbid{1};
  uint32_t dbapp_id{0};
  Address dbapp_addr;
  bool is_retiring{false};

  void Serialize(BinaryWriter& w) const {
    w.Write(low_dbid);
    w.Write(high_dbid);
    w.Write(dbapp_id);
    w.Write(dbapp_addr.Ip());
    w.Write(dbapp_addr.Port());
    w.Write<uint8_t>(is_retiring ? 1u : 0u);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShardEntry> {
    auto low = r.Read<DatabaseID>();
    auto high = r.Read<DatabaseID>();
    auto id = r.Read<uint32_t>();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto retiring = r.Read<uint8_t>();
    if (!low || !high || !id || !ip || !port || !retiring) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::ShardEntry: truncated"};
    }
    if (*low <= kInvalidDBID || *high <= *low || *id == 0 || *retiring > 1) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::ShardEntry: invalid"};
    }
    ShardEntry msg;
    msg.low_dbid = *low;
    msg.high_dbid = *high;
    msg.dbapp_id = *id;
    msg.dbapp_addr = Address(*ip, *port);
    msg.is_retiring = (*retiring != 0);
    return msg;
  }
};

struct RegisterDbApp {
  Address internal_addr;
  uint32_t known_app_id{0};
  uint32_t known_shard_table_version{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::DBAppMgr::kRegisterDbApp),
        "dbappmgr::RegisterDbApp",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t) * 2),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
    w.Write(known_app_id);
    w.Write(known_shard_table_version);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterDbApp> {
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto known_id = r.Read<uint32_t>();
    auto version = r.Read<uint32_t>();
    if (!ip || !port || !known_id || !version) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::RegisterDbApp: truncated"};
    }
    RegisterDbApp msg;
    msg.internal_addr = Address(*ip, *port);
    msg.known_app_id = *known_id;
    msg.known_shard_table_version = *version;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterDbApp>);

struct RegisterDbAppAck {
  bool success{false};
  uint32_t dbapp_id{0};
  uint32_t shard_table_version{0};
  std::vector<ShardEntry> entries;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kRegisterDbAppAck),
                                   "dbappmgr::RegisterDbAppAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(success ? 1u : 0u);
    w.Write(dbapp_id);
    w.Write(shard_table_version);
    w.Write(static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) entry.Serialize(w);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterDbAppAck> {
    auto ok = r.Read<uint8_t>();
    auto id = r.Read<uint32_t>();
    auto version = r.Read<uint32_t>();
    auto count = r.Read<uint32_t>();
    if (!ok || !id || !version || !count) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::RegisterDbAppAck: truncated"};
    }
    if (*ok > 1 || *count > kMaxEntries) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::RegisterDbAppAck: invalid"};
    }
    RegisterDbAppAck msg;
    msg.success = (*ok != 0);
    msg.dbapp_id = *id;
    msg.shard_table_version = *version;
    msg.entries.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto entry = ShardEntry::Deserialize(r);
      if (!entry) return entry.Error();
      msg.entries.push_back(*entry);
    }
    return msg;
  }

 private:
  static constexpr uint32_t kMaxEntries = 4096;
};
static_assert(NetworkMessage<RegisterDbAppAck>);

struct InformLoad {
  uint32_t dbapp_id{0};
  float load{0.0f};
  uint32_t entity_count{0};
  uint32_t pending_checkout_count{0};
  uint32_t write_queue_depth{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::DBAppMgr::kInformLoad),
        "dbappmgr::InformLoad",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(float) + sizeof(uint32_t) * 3),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(dbapp_id);
    w.Write(load);
    w.Write(entity_count);
    w.Write(pending_checkout_count);
    w.Write(write_queue_depth);
  }

  static auto Deserialize(BinaryReader& r) -> Result<InformLoad> {
    auto id = r.Read<uint32_t>();
    auto ld = r.Read<float>();
    auto entities = r.Read<uint32_t>();
    auto pending = r.Read<uint32_t>();
    auto queued = r.Read<uint32_t>();
    if (!id || !ld || !entities || !pending || !queued) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::InformLoad: truncated"};
    }
    InformLoad msg;
    msg.dbapp_id = *id;
    msg.load = *ld;
    msg.entity_count = *entities;
    msg.pending_checkout_count = *pending;
    msg.write_queue_depth = *queued;
    return msg;
  }
};
static_assert(NetworkMessage<InformLoad>);

struct GetShardTable {
  uint32_t request_id{0};
  uint32_t known_version{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kGetShardTable),
                                   "dbappmgr::GetShardTable",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) * 2),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(known_version);
  }

  static auto Deserialize(BinaryReader& r) -> Result<GetShardTable> {
    auto request = r.Read<uint32_t>();
    auto version = r.Read<uint32_t>();
    if (!request || !version) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::GetShardTable: truncated"};
    }
    return GetShardTable{*request, *version};
  }
};
static_assert(NetworkMessage<GetShardTable>);

struct ShardTableResponse {
  uint32_t request_id{0};
  uint32_t version{0};
  std::vector<ShardEntry> entries;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kShardTableResponse),
                                   "dbappmgr::ShardTableResponse",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(version);
    w.Write(static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) entry.Serialize(w);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShardTableResponse> {
    auto request = r.Read<uint32_t>();
    auto version = r.Read<uint32_t>();
    auto count = r.Read<uint32_t>();
    if (!request || !version || !count || *count > kMaxEntries) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::ShardTableResponse: invalid"};
    }
    ShardTableResponse msg;
    msg.request_id = *request;
    msg.version = *version;
    msg.entries.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto entry = ShardEntry::Deserialize(r);
      if (!entry) return entry.Error();
      msg.entries.push_back(*entry);
    }
    return msg;
  }

 private:
  static constexpr uint32_t kMaxEntries = 4096;
};
static_assert(NetworkMessage<ShardTableResponse>);

struct ShardTableUpdate {
  uint32_t version{0};
  std::vector<ShardEntry> entries;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kShardTableUpdate),
                                   "dbappmgr::ShardTableUpdate",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(version);
    w.Write(static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) entry.Serialize(w);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShardTableUpdate> {
    auto version = r.Read<uint32_t>();
    auto count = r.Read<uint32_t>();
    if (!version || !count || *count > kMaxEntries) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::ShardTableUpdate: invalid"};
    }
    ShardTableUpdate msg;
    msg.version = *version;
    msg.entries.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto entry = ShardEntry::Deserialize(r);
      if (!entry) return entry.Error();
      msg.entries.push_back(*entry);
    }
    return msg;
  }

 private:
  static constexpr uint32_t kMaxEntries = 4096;
};
static_assert(NetworkMessage<ShardTableUpdate>);

struct RecoverDBAppState {
  uint32_t dbapp_id{0};
  uint32_t shard_table_version{0};
  std::vector<ShardEntry> shards;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kRecoverDBAppState),
                                   "dbappmgr::RecoverDBAppState",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(dbapp_id);
    w.Write(shard_table_version);
    w.Write(static_cast<uint32_t>(shards.size()));
    for (const auto& shard : shards) shard.Serialize(w);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RecoverDBAppState> {
    auto id = r.Read<uint32_t>();
    auto version = r.Read<uint32_t>();
    auto count = r.Read<uint32_t>();
    if (!id || !version || !count || *id == 0 || *version == 0 || *count > kMaxEntries) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::RecoverDBAppState: invalid"};
    }
    RecoverDBAppState msg;
    msg.dbapp_id = *id;
    msg.shard_table_version = *version;
    msg.shards.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      auto shard = ShardEntry::Deserialize(r);
      if (!shard) return shard.Error();
      msg.shards.push_back(*shard);
    }
    return msg;
  }

 private:
  static constexpr uint32_t kMaxEntries = 4096;
};
static_assert(NetworkMessage<RecoverDBAppState>);

struct HealthProbe {
  uint64_t nonce{0};
  uint8_t reviver_priority{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kHealthProbe),
                                   "dbappmgr::HealthProbe",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint64_t) + sizeof(uint8_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(nonce);
    w.Write(reviver_priority);
  }

  static auto Deserialize(BinaryReader& r) -> Result<HealthProbe> {
    auto value = r.Read<uint64_t>();
    auto priority = r.Read<uint8_t>();
    if (!value || !priority) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::HealthProbe: truncated"};
    }
    return HealthProbe{*value, *priority};
  }
};
static_assert(NetworkMessage<HealthProbe>);

struct HealthProbeAck {
  uint64_t nonce{0};
  uint64_t game_time{0};
  bool is_active_reviver{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBAppMgr::kHealthProbeAck),
                                   "dbappmgr::HealthProbeAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(nonce);
    w.Write(game_time);
    w.Write<uint8_t>(is_active_reviver ? 1u : 0u);
  }

  static auto Deserialize(BinaryReader& r) -> Result<HealthProbeAck> {
    auto value = r.Read<uint64_t>();
    auto tick = r.Read<uint64_t>();
    auto active = r.Read<uint8_t>();
    if (!value || !tick || !active || *active > 1) {
      return Error{ErrorCode::kInvalidArgument, "dbappmgr::HealthProbeAck: invalid"};
    }
    return HealthProbeAck{*value, *tick, *active != 0};
  }
};
static_assert(NetworkMessage<HealthProbeAck>);

}  // namespace atlas::dbappmgr

#endif  // ATLAS_SERVER_DBAPPMGR_DBAPPMGR_MESSAGES_H_
