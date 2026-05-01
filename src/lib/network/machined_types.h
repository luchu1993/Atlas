#ifndef ATLAS_LIB_NETWORK_MACHINED_TYPES_H_
#define ATLAS_LIB_NETWORK_MACHINED_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "foundation/process_type.h"
#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"

namespace atlas::machined {

struct ProcessInfo {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  Address internal_addr;
  Address external_addr;
  uint32_t pid{0};
  float load{0.0f};
};

inline constexpr uint8_t kProtocolVersion = 1;

struct RegisterMessage {
  uint8_t protocol_version{kProtocolVersion};
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  uint16_t internal_port{0};
  uint16_t external_port{0};  // 0 = none
  uint32_t pid{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Machined::kRegister), "machined::Register",
        MessageLengthStyle::kVariable,           -1,
        MessageReliability::kReliable,           MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(protocol_version);
    w.Write<uint8_t>(static_cast<uint8_t>(process_type));
    w.WriteString(name);
    w.Write<uint16_t>(internal_port);
    w.Write<uint16_t>(external_port);
    w.Write<uint32_t>(pid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterMessage> {
    RegisterMessage msg;
    auto ver = r.Read<uint8_t>();
    if (!ver) return ver.Error();
    msg.protocol_version = *ver;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.process_type = static_cast<ProcessType>(*pt);
    auto name = r.ReadString();
    if (!name) return name.Error();
    msg.name = std::move(*name);
    auto iport = r.Read<uint16_t>();
    if (!iport) return iport.Error();
    msg.internal_port = *iport;
    auto eport = r.Read<uint16_t>();
    if (!eport) return eport.Error();
    msg.external_port = *eport;
    auto pid = r.Read<uint32_t>();
    if (!pid) return pid.Error();
    msg.pid = *pid;
    return msg;
  }
};

struct RegisterAck {
  bool success{true};
  std::string error_message;
  uint64_t server_time{0};         // machined current time (ms since epoch)
  uint16_t heartbeat_udp_port{0};  // UDP port for heartbeat datagrams (0 = use TCP)

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kRegisterAck),
                                   "machined::RegisterAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(success ? 1 : 0);
    w.WriteString(error_message);
    w.Write<uint64_t>(server_time);
    w.Write<uint16_t>(heartbeat_udp_port);
  }

  static auto Deserialize(BinaryReader& r) -> Result<RegisterAck> {
    RegisterAck msg;
    auto ok = r.Read<uint8_t>();
    if (!ok) return ok.Error();
    msg.success = (*ok != 0);
    auto err = r.ReadString();
    if (!err) return err.Error();
    msg.error_message = std::move(*err);
    auto ts = r.Read<uint64_t>();
    if (!ts) return ts.Error();
    msg.server_time = *ts;
    // Optional for older machined replies.
    auto udp_port = r.Read<uint16_t>();
    msg.heartbeat_udp_port = udp_port ? *udp_port : 0;
    return msg;
  }
};

struct DeregisterMessage {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  uint32_t pid{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kDeregister),
                                   "machined::Deregister",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(process_type));
    w.WriteString(name);
    w.Write<uint32_t>(pid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<DeregisterMessage> {
    DeregisterMessage msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.process_type = static_cast<ProcessType>(*pt);
    auto name = r.ReadString();
    if (!name) return name.Error();
    msg.name = std::move(*name);
    auto pid = r.Read<uint32_t>();
    if (!pid) return pid.Error();
    msg.pid = *pid;
    return msg;
  }
};

struct QueryMessage {
  ProcessType process_type{ProcessType::kBaseApp};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Machined::kQuery), "machined::Query",
        MessageLengthStyle::kVariable,        -1,
        MessageReliability::kReliable,        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint8_t>(static_cast<uint8_t>(process_type)); }

  static auto Deserialize(BinaryReader& r) -> Result<QueryMessage> {
    QueryMessage msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.process_type = static_cast<ProcessType>(*pt);
    return msg;
  }
};

struct QueryResponse {
  std::vector<ProcessInfo> processes;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kQueryResponse),
                                   "machined::QueryResponse",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(static_cast<uint32_t>(processes.size()));
    for (const auto& p : processes) {
      w.Write<uint8_t>(static_cast<uint8_t>(p.process_type));
      w.WriteString(p.name);
      w.Write<uint32_t>(p.internal_addr.Ip());
      w.Write<uint16_t>(p.internal_addr.Port());
      w.Write<uint32_t>(p.external_addr.Ip());
      w.Write<uint16_t>(p.external_addr.Port());
      w.Write<uint32_t>(p.pid);
      w.Write<float>(p.load);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<QueryResponse> {
    auto count = r.Read<uint32_t>();
    if (!count) return count.Error();

    constexpr uint32_t kMaxProcesses = 10000;
    if (*count > kMaxProcesses) {
      return Error{ErrorCode::kInvalidArgument, "QueryResponse: process count exceeds limit"};
    }

    QueryResponse resp;
    resp.processes.reserve(*count);
    for (uint32_t i = 0; i < *count; ++i) {
      ProcessInfo p;
      auto pt = r.Read<uint8_t>();
      if (!pt) return pt.Error();
      p.process_type = static_cast<ProcessType>(*pt);
      auto name = r.ReadString();
      if (!name) return name.Error();
      p.name = std::move(*name);
      auto iip = r.Read<uint32_t>();
      if (!iip) return iip.Error();
      auto iport = r.Read<uint16_t>();
      if (!iport) return iport.Error();
      p.internal_addr = Address(*iip, *iport);
      auto eip = r.Read<uint32_t>();
      if (!eip) return eip.Error();
      auto eport = r.Read<uint16_t>();
      if (!eport) return eport.Error();
      p.external_addr = Address(*eip, *eport);
      auto pid = r.Read<uint32_t>();
      if (!pid) return pid.Error();
      p.pid = *pid;
      auto load = r.Read<float>();
      if (!load) return load.Error();
      p.load = *load;
      resp.processes.push_back(std::move(p));
    }
    return resp;
  }
};

struct HeartbeatMessage {
  float load{0.0f};
  uint32_t entity_count{0};
  uint32_t pid{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kHeartbeat),
                                   "machined::Heartbeat",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<float>(load);
    w.Write<uint32_t>(entity_count);
    w.Write<uint32_t>(pid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<HeartbeatMessage> {
    HeartbeatMessage msg;
    auto load = r.Read<float>();
    if (!load) return load.Error();
    msg.load = *load;
    auto ec = r.Read<uint32_t>();
    if (!ec) return ec.Error();
    msg.entity_count = *ec;
    auto pid = r.Read<uint32_t>();
    msg.pid = pid ? *pid : 0;
    return msg;
  }
};

struct HeartbeatAck {
  uint64_t server_time{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kHeartbeatAck),
                                   "machined::HeartbeatAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint64_t>(server_time); }

  static auto Deserialize(BinaryReader& r) -> Result<HeartbeatAck> {
    HeartbeatAck msg;
    auto ts = r.Read<uint64_t>();
    if (!ts) return ts.Error();
    msg.server_time = *ts;
    return msg;
  }
};

struct BirthNotification {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  Address internal_addr;
  Address external_addr;
  uint32_t pid{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kBirthNotification),
                                   "machined::BirthNotification",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(process_type));
    w.WriteString(name);
    w.Write<uint32_t>(internal_addr.Ip());
    w.Write<uint16_t>(internal_addr.Port());
    w.Write<uint32_t>(external_addr.Ip());
    w.Write<uint16_t>(external_addr.Port());
    w.Write<uint32_t>(pid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<BirthNotification> {
    BirthNotification msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.process_type = static_cast<ProcessType>(*pt);
    auto name = r.ReadString();
    if (!name) return name.Error();
    msg.name = std::move(*name);
    auto iip = r.Read<uint32_t>();
    if (!iip) return iip.Error();
    auto iport = r.Read<uint16_t>();
    if (!iport) return iport.Error();
    msg.internal_addr = Address(*iip, *iport);
    auto eip = r.Read<uint32_t>();
    if (!eip) return eip.Error();
    auto eport = r.Read<uint16_t>();
    if (!eport) return eport.Error();
    msg.external_addr = Address(*eip, *eport);
    auto pid = r.Read<uint32_t>();
    if (!pid) return pid.Error();
    msg.pid = *pid;
    return msg;
  }
};

struct DeathNotification {
  ProcessType process_type{ProcessType::kBaseApp};
  std::string name;
  Address internal_addr;
  uint8_t reason{0};  // 0=normal, 1=connection_lost, 2=timeout

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kDeathNotification),
                                   "machined::DeathNotification",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(process_type));
    w.WriteString(name);
    w.Write<uint32_t>(internal_addr.Ip());
    w.Write<uint16_t>(internal_addr.Port());
    w.Write<uint8_t>(reason);
  }

  static auto Deserialize(BinaryReader& r) -> Result<DeathNotification> {
    DeathNotification msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.process_type = static_cast<ProcessType>(*pt);
    auto name = r.ReadString();
    if (!name) return name.Error();
    msg.name = std::move(*name);
    auto iip = r.Read<uint32_t>();
    if (!iip) return iip.Error();
    auto iport = r.Read<uint16_t>();
    if (!iport) return iport.Error();
    msg.internal_addr = Address(*iip, *iport);
    auto reason = r.Read<uint8_t>();
    if (!reason) return reason.Error();
    msg.reason = *reason;
    return msg;
  }
};

enum class ListenerType : uint8_t {
  kBirth = 0,
  kDeath = 1,
  kBoth = 2,
};

struct ListenerRegister {
  ListenerType listener_type{ListenerType::kBoth};
  ProcessType target_type{ProcessType::kBaseApp};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kListenerRegister),
                                   "machined::ListenerRegister",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(listener_type));
    w.Write<uint8_t>(static_cast<uint8_t>(target_type));
  }

  static auto Deserialize(BinaryReader& r) -> Result<ListenerRegister> {
    ListenerRegister msg;
    auto lt = r.Read<uint8_t>();
    if (!lt) return lt.Error();
    msg.listener_type = static_cast<ListenerType>(*lt);
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.target_type = static_cast<ProcessType>(*pt);
    return msg;
  }
};

struct ListenerAck {
  bool success{true};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kListenerAck),
                                   "machined::ListenerAck",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const { w.Write<uint8_t>(success ? 1 : 0); }

  static auto Deserialize(BinaryReader& r) -> Result<ListenerAck> {
    ListenerAck msg;
    auto ok = r.Read<uint8_t>();
    if (!ok) return ok.Error();
    msg.success = (*ok != 0);
    return msg;
  }
};

struct WatcherRequest {
  ProcessType target_type{ProcessType::kBaseApp};
  std::string target_name;  // empty = all instances of target_type
  std::string watcher_path;
  uint32_t request_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kWatcherRequest),
                                   "machined::WatcherRequest",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(target_type));
    w.WriteString(target_name);
    w.WriteString(watcher_path);
    w.Write<uint32_t>(request_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WatcherRequest> {
    WatcherRequest msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.target_type = static_cast<ProcessType>(*pt);
    auto tname = r.ReadString();
    if (!tname) return tname.Error();
    msg.target_name = std::move(*tname);
    auto path = r.ReadString();
    if (!path) return path.Error();
    msg.watcher_path = std::move(*path);
    auto rid = r.Read<uint32_t>();
    if (!rid) return rid.Error();
    msg.request_id = *rid;
    return msg;
  }
};

struct WatcherResponse {
  uint32_t request_id{0};
  bool found{false};
  std::string source_name;
  std::string value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kWatcherResponse),
                                   "machined::WatcherResponse",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(request_id);
    w.Write<uint8_t>(found ? 1 : 0);
    w.WriteString(source_name);
    w.WriteString(value);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WatcherResponse> {
    WatcherResponse msg;
    auto rid = r.Read<uint32_t>();
    if (!rid) return rid.Error();
    msg.request_id = *rid;
    auto found = r.Read<uint8_t>();
    if (!found) return found.Error();
    msg.found = (*found != 0);
    auto sname = r.ReadString();
    if (!sname) return sname.Error();
    msg.source_name = std::move(*sname);
    auto val = r.ReadString();
    if (!val) return val.Error();
    msg.value = std::move(*val);
    return msg;
  }
};

struct WatcherForward {
  uint32_t request_id{0};
  std::string requester_name;
  std::string watcher_path;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kWatcherForward),
                                   "machined::WatcherForward",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(request_id);
    w.WriteString(requester_name);
    w.WriteString(watcher_path);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WatcherForward> {
    WatcherForward msg;
    auto rid = r.Read<uint32_t>();
    if (!rid) return rid.Error();
    msg.request_id = *rid;
    auto rname = r.ReadString();
    if (!rname) return rname.Error();
    msg.requester_name = std::move(*rname);
    auto path = r.ReadString();
    if (!path) return path.Error();
    msg.watcher_path = std::move(*path);
    return msg;
  }
};

struct WatcherReply {
  uint32_t request_id{0};
  bool found{false};
  std::string value;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kWatcherReply),
                                   "machined::WatcherReply",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint32_t>(request_id);
    w.Write<uint8_t>(found ? 1 : 0);
    w.WriteString(value);
  }

  static auto Deserialize(BinaryReader& r) -> Result<WatcherReply> {
    WatcherReply msg;
    auto rid = r.Read<uint32_t>();
    if (!rid) return rid.Error();
    msg.request_id = *rid;
    auto found = r.Read<uint8_t>();
    if (!found) return found.Error();
    msg.found = (*found != 0);
    auto val = r.ReadString();
    if (!val) return val.Error();
    msg.value = std::move(*val);
    return msg;
  }
};

struct ShutdownTarget {
  ProcessType target_type{ProcessType::kBaseApp};
  std::string target_name;  // empty = all instances of target_type
  uint8_t reason{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Machined::kShutdownTarget),
                                   "machined::ShutdownTarget",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(target_type));
    w.WriteString(target_name);
    w.Write<uint8_t>(reason);
  }

  static auto Deserialize(BinaryReader& r) -> Result<ShutdownTarget> {
    ShutdownTarget msg;
    auto pt = r.Read<uint8_t>();
    if (!pt) return pt.Error();
    msg.target_type = static_cast<ProcessType>(*pt);
    auto name = r.ReadString();
    if (!name) return name.Error();
    msg.target_name = std::move(*name);
    auto reason = r.Read<uint8_t>();
    if (!reason) return reason.Error();
    msg.reason = *reason;
    return msg;
  }
};

static_assert(NetworkMessage<RegisterMessage>);
static_assert(NetworkMessage<RegisterAck>);
static_assert(NetworkMessage<DeregisterMessage>);
static_assert(NetworkMessage<QueryMessage>);
static_assert(NetworkMessage<QueryResponse>);
static_assert(NetworkMessage<HeartbeatMessage>);
static_assert(NetworkMessage<HeartbeatAck>);
static_assert(NetworkMessage<BirthNotification>);
static_assert(NetworkMessage<DeathNotification>);
static_assert(NetworkMessage<ListenerRegister>);
static_assert(NetworkMessage<ListenerAck>);
static_assert(NetworkMessage<WatcherRequest>);
static_assert(NetworkMessage<WatcherResponse>);
static_assert(NetworkMessage<WatcherForward>);
static_assert(NetworkMessage<WatcherReply>);
static_assert(NetworkMessage<ShutdownTarget>);

}  // namespace atlas::machined

#endif  // ATLAS_LIB_NETWORK_MACHINED_TYPES_H_
