#ifndef ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_
#define ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_

#include <cstdint>
#include <string>

#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

// ============================================================================
// BaseAppMgr messages (IDs 6000–6012)
//
// Directions:
//   BaseApp    → BaseAppMgr : RegisterBaseApp     (6000)
//   BaseAppMgr → BaseApp    : RegisterBaseAppAck  (6001)
//   BaseApp    → BaseAppMgr : BaseAppReady        (6002)
//   BaseApp    → BaseAppMgr : InformLoad          (6003)
//   BaseApp    → BaseAppMgr : RegisterGlobalBase  (6010)
//   BaseApp    → BaseAppMgr : DeregisterGlobalBase(6011)
//   BaseAppMgr → BaseApp    : GlobalBaseNotification (6012)
// ============================================================================

namespace atlas::baseappmgr {

// ============================================================================
// RegisterBaseApp  (BaseApp → BaseAppMgr, ID 6000)
// ============================================================================

struct RegisterBaseApp {
  Address internal_addr;
  Address external_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kRegisterBaseApp),
                                   "baseappmgr::RegisterBaseApp",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>((sizeof(uint32_t) + sizeof(uint16_t)) * 2),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
    w.Write(external_addr.Ip());
    w.Write(external_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterBaseApp> {
    auto iip = r.Read<uint32_t>();
    auto iport = r.Read<uint16_t>();
    auto eip = r.Read<uint32_t>();
    auto eport = r.Read<uint16_t>();
    if (!iip || !iport || !eip || !eport)
      return Error{ErrorCode::kInvalidArgument, "RegisterBaseApp: truncated"};
    RegisterBaseApp msg;
    msg.internal_addr = Address(*iip, *iport);
    msg.external_addr = Address(*eip, *eport);
    return msg;
  }
};
static_assert(NetworkMessage<RegisterBaseApp>);

// ============================================================================
// RegisterBaseAppAck  (BaseAppMgr → BaseApp, ID 6001)
// ============================================================================

struct RegisterBaseAppAck {
  bool success{false};
  uint32_t app_id{0};
  uint64_t game_time{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseAppMgr::kRegisterBaseAppAck),
        "baseappmgr::RegisterBaseAppAck",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(app_id);
    w.Write(game_time);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterBaseAppAck> {
    auto ok = r.Read<uint8_t>();
    auto aid = r.Read<uint32_t>();
    auto gt = r.Read<uint64_t>();
    if (!ok || !aid || !gt)
      return Error{ErrorCode::kInvalidArgument, "RegisterBaseAppAck: truncated"};
    RegisterBaseAppAck msg;
    msg.success = (*ok != 0);
    msg.app_id = *aid;
    msg.game_time = *gt;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterBaseAppAck>);

// ============================================================================
// BaseAppReady  (BaseApp → BaseAppMgr, ID 6002)
// ============================================================================

struct BaseAppReady {
  uint32_t app_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kBaseAppReady),
                                   "baseappmgr::BaseAppReady",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write(app_id); }

  static auto Deserialize(BinaryReader& r) -> Result<BaseAppReady> {
    auto aid = r.Read<uint32_t>();
    if (!aid) return Error{ErrorCode::kInvalidArgument, "BaseAppReady: truncated"};
    BaseAppReady msg;
    msg.app_id = *aid;
    return msg;
  }
};
static_assert(NetworkMessage<BaseAppReady>);

// ============================================================================
// InformLoad  (BaseApp → BaseAppMgr, ID 6003)
// ============================================================================

struct InformLoad {
  uint32_t app_id{0};
  float load{0.0f};
  uint32_t entity_count{0};
  uint32_t proxy_count{0};
  uint32_t pending_prepare_count{0};
  uint32_t pending_force_logoff_count{0};
  uint32_t detached_proxy_count{0};
  uint32_t logoff_in_flight_count{0};
  uint32_t deferred_login_count{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseAppMgr::kInformLoad),
        "baseappmgr::InformLoad",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(float) + sizeof(uint32_t) * 7),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(app_id);
    w.Write(load);
    w.Write(entity_count);
    w.Write(proxy_count);
    w.Write(pending_prepare_count);
    w.Write(pending_force_logoff_count);
    w.Write(detached_proxy_count);
    w.Write(logoff_in_flight_count);
    w.Write(deferred_login_count);
  }

  static auto Deserialize(BinaryReader& r) -> Result<InformLoad> {
    auto aid = r.Read<uint32_t>();
    auto ld = r.Read<float>();
    auto ec = r.Read<uint32_t>();
    auto pc = r.Read<uint32_t>();
    auto ppc = r.Read<uint32_t>();
    auto pfl = r.Read<uint32_t>();
    auto dpc = r.Read<uint32_t>();
    auto lif = r.Read<uint32_t>();
    auto dlc = r.Read<uint32_t>();
    if (!aid || !ld || !ec || !pc || !ppc || !pfl || !dpc || !lif || !dlc)
      return Error{ErrorCode::kInvalidArgument, "InformLoad: truncated"};
    InformLoad msg;
    msg.app_id = *aid;
    msg.load = *ld;
    msg.entity_count = *ec;
    msg.proxy_count = *pc;
    msg.pending_prepare_count = *ppc;
    msg.pending_force_logoff_count = *pfl;
    msg.detached_proxy_count = *dpc;
    msg.logoff_in_flight_count = *lif;
    msg.deferred_login_count = *dlc;
    return msg;
  }
};
static_assert(NetworkMessage<InformLoad>);

// ============================================================================
// RegisterGlobalBase  (BaseApp → BaseAppMgr, ID 6010)
// ============================================================================

struct RegisterGlobalBase {
  std::string key;
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kRegisterGlobalBase),
                                   "baseappmgr::RegisterGlobalBase",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WriteString(key);
    w.Write(entity_id);
    w.Write(type_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterGlobalBase> {
    auto k = r.ReadString();
    auto eid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    if (!k || !eid || !ti)
      return Error{ErrorCode::kInvalidArgument, "RegisterGlobalBase: truncated"};
    RegisterGlobalBase msg;
    msg.key = std::move(*k);
    msg.entity_id = *eid;
    msg.type_id = *ti;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterGlobalBase>);

// ============================================================================
// DeregisterGlobalBase  (BaseApp → BaseAppMgr, ID 6011)
// ============================================================================

struct DeregisterGlobalBase {
  std::string key;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kDeregisterGlobalBase),
                                   "baseappmgr::DeregisterGlobalBase",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.WriteString(key); }

  static auto Deserialize(BinaryReader& r) -> Result<DeregisterGlobalBase> {
    auto k = r.ReadString();
    if (!k) return Error{ErrorCode::kInvalidArgument, "DeregisterGlobalBase: truncated"};
    DeregisterGlobalBase msg;
    msg.key = std::move(*k);
    return msg;
  }
};
static_assert(NetworkMessage<DeregisterGlobalBase>);

// ============================================================================
// GlobalBaseNotification  (BaseAppMgr → BaseApp, ID 6012)
// ============================================================================

struct GlobalBaseNotification {
  std::string key;
  Address base_addr;
  EntityID entity_id{kInvalidEntityID};
  uint16_t type_id{0};
  bool added{true};  // true=registered, false=deregistered

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kGlobalBaseNotification),
                                   "baseappmgr::GlobalBaseNotification",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WriteString(key);
    w.Write(base_addr.Ip());
    w.Write(base_addr.Port());
    w.Write(entity_id);
    w.Write(type_id);
    w.Write(static_cast<uint8_t>(added ? 1 : 0));
  }

  static auto Deserialize(BinaryReader& r) -> Result<GlobalBaseNotification> {
    auto k = r.ReadString();
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto eid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto add = r.Read<uint8_t>();
    if (!k || !ip || !port || !eid || !ti || !add)
      return Error{ErrorCode::kInvalidArgument, "GlobalBaseNotification: truncated"};
    GlobalBaseNotification msg;
    msg.key = std::move(*k);
    msg.base_addr = Address(*ip, *port);
    msg.entity_id = *eid;
    msg.type_id = *ti;
    msg.added = (*add != 0);
    return msg;
  }
};
static_assert(NetworkMessage<GlobalBaseNotification>);

}  // namespace atlas::baseappmgr

#endif  // ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_
