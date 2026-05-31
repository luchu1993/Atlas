#ifndef ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_
#define ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_

#include <cstdint>
#include <string>

#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"

namespace atlas::baseappmgr {

struct RegisterBaseApp {
  Address internal_addr;
  Address external_addr;
  // Echo of the BaseApp's current app_id (0 if never assigned). A manager that
  // restarted without our entry keeps it so InformLoad routing stays stable.
  uint32_t known_app_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::BaseAppMgr::kRegisterBaseApp),
        "baseappmgr::RegisterBaseApp",
        MessageLengthStyle::kFixed,
        static_cast<int>((sizeof(uint32_t) + sizeof(uint16_t)) * 2 + sizeof(uint32_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
    w.Write(external_addr.Ip());
    w.Write(external_addr.Port());
    w.Write(known_app_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterBaseApp> {
    auto iip = r.Read<uint32_t>();
    auto iport = r.Read<uint16_t>();
    auto eip = r.Read<uint32_t>();
    auto eport = r.Read<uint16_t>();
    auto known = r.Read<uint32_t>();
    if (!iip || !iport || !eip || !eport || !known)
      return Error{ErrorCode::kInvalidArgument, "RegisterBaseApp: truncated"};
    RegisterBaseApp msg;
    msg.internal_addr = Address(*iip, *iport);
    msg.external_addr = Address(*eip, *eport);
    msg.known_app_id = *known;
    return msg;
  }
};
static_assert(NetworkMessage<RegisterBaseApp>);

struct RegisterBaseAppAck {
  bool success{false};
  uint32_t app_id{0};
  uint64_t game_time{0};

  static auto Descriptor() -> const MessageDesc& {
    // kVariable so future trailing fields ride a packed-length prefix and
    // old/new peers stay deserialize-compatible.
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kRegisterBaseAppAck),
                                   "baseappmgr::RegisterBaseAppAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
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

struct HealthProbe {
  uint64_t nonce{0};
  // Static ReviverPriority; the subject arbitrates the active monitor by it.
  uint8_t reviver_priority{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kHealthProbe),
                                   "baseappmgr::HealthProbe",
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
    if (!value || !priority)
      return Error{ErrorCode::kInvalidArgument, "baseappmgr::HealthProbe: truncated"};
    HealthProbe msg;
    msg.nonce = *value;
    msg.reviver_priority = *priority;
    return msg;
  }
};
static_assert(NetworkMessage<HealthProbe>);

struct HealthProbeAck {
  uint64_t nonce{0};
  uint64_t game_time{0};
  // Subject's verdict: is the pinging Reviver the active monitor?
  bool is_active_reviver{false};

  static auto Descriptor() -> const MessageDesc& {
    // kVariable so future trailing fields stay deserialize-compatible.
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseAppMgr::kHealthProbeAck),
                                   "baseappmgr::HealthProbeAck",
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
    if (!value || !tick || !active) {
      return Error{ErrorCode::kInvalidArgument, "baseappmgr::HealthProbeAck: truncated"};
    }
    if (*active > 1) return Error{ErrorCode::kInvalidArgument, "baseappmgr::HealthProbeAck: bad flag"};
    HealthProbeAck msg;
    msg.nonce = *value;
    msg.game_time = *tick;
    msg.is_active_reviver = (*active != 0);
    return msg;
  }
};
static_assert(NetworkMessage<HealthProbeAck>);

}  // namespace atlas::baseappmgr

#endif  // ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_MESSAGES_H_
