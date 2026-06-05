#ifndef ATLAS_LIB_NETWORK_MESH_GOSSIP_H_
#define ATLAS_LIB_NETWORK_MESH_GOSSIP_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "foundation/error.h"
#include "network/address.h"
#include "network/machined_types.h"
#include "serialization/binary_stream.h"

namespace atlas::machined {

inline constexpr uint8_t kMeshProtocolVersion = 1;

// Raw broadcast datagrams between per-host machined daemons (not Channel
// messages); each is self-framed by a leading MeshMessageType tag.
enum class MeshMessageType : uint8_t {
  kHello = 1,
  kRegistry = 2,
  kProcessDeath = 3,
};

// Periodic discovery broadcast: peers learn the sender's mesh endpoint, and a
// fresh incarnation on a known address signals a restart that warrants a
// registry resync.
struct MeshHello {
  uint8_t protocol_version{kMeshProtocolVersion};
  Address machined_addr;
  uint64_t incarnation{0};

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(MeshMessageType::kHello));
    w.Write<uint8_t>(protocol_version);
    w.Write<uint32_t>(machined_addr.Ip());
    w.Write<uint16_t>(machined_addr.Port());
    w.Write<uint64_t>(incarnation);
  }

  static auto Deserialize(BinaryReader& r) -> Result<MeshHello> {
    auto type = r.Read<uint8_t>();
    if (!type) return type.Error();
    if (static_cast<MeshMessageType>(*type) != MeshMessageType::kHello) {
      return Error{ErrorCode::kInvalidArgument, "MeshHello: unexpected message type"};
    }
    MeshHello msg;
    auto ver = r.Read<uint8_t>();
    if (!ver) return ver.Error();
    if (*ver != kMeshProtocolVersion) {
      return Error{ErrorCode::kInvalidArgument, "MeshHello: incompatible protocol version"};
    }
    msg.protocol_version = *ver;
    auto ip = r.Read<uint32_t>();
    if (!ip) return ip.Error();
    auto port = r.Read<uint16_t>();
    if (!port) return port.Error();
    msg.machined_addr = Address(*ip, *port);
    auto inc = r.Read<uint64_t>();
    if (!inc) return inc.Error();
    msg.incarnation = *inc;
    return msg;
  }
};

// Periodic gossip of one machined's local process table; receivers replace that
// owner's entries in their MeshRegistry. Mirrors QueryResponse's ProcessInfo
// encoding.
struct MeshRegistryMsg {
  uint8_t protocol_version{kMeshProtocolVersion};
  Address owner;
  std::vector<ProcessInfo> processes;

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(MeshMessageType::kRegistry));
    w.Write<uint8_t>(protocol_version);
    w.Write<uint32_t>(owner.Ip());
    w.Write<uint16_t>(owner.Port());
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

  static auto Deserialize(BinaryReader& r) -> Result<MeshRegistryMsg> {
    auto type = r.Read<uint8_t>();
    if (!type) return type.Error();
    if (static_cast<MeshMessageType>(*type) != MeshMessageType::kRegistry) {
      return Error{ErrorCode::kInvalidArgument, "MeshRegistryMsg: unexpected message type"};
    }
    auto ver = r.Read<uint8_t>();
    if (!ver) return ver.Error();
    if (*ver != kMeshProtocolVersion) {
      return Error{ErrorCode::kInvalidArgument, "MeshRegistryMsg: incompatible protocol version"};
    }
    MeshRegistryMsg msg;
    msg.protocol_version = *ver;
    auto oip = r.Read<uint32_t>();
    if (!oip) return oip.Error();
    auto oport = r.Read<uint16_t>();
    if (!oport) return oport.Error();
    msg.owner = Address(*oip, *oport);
    auto count = r.Read<uint32_t>();
    if (!count) return count.Error();
    constexpr uint32_t kMaxProcesses = 10000;
    if (*count > kMaxProcesses) {
      return Error{ErrorCode::kInvalidArgument, "MeshRegistryMsg: process count exceeds limit"};
    }
    msg.processes.reserve(*count);
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
      msg.processes.push_back(std::move(p));
    }
    return msg;
  }
};

// A machined's ring predecessor broadcasts this when the machined stops sending
// HELLOs, so every node drops that owner's processes at once instead of waiting
// out its own membership timeout.
struct MeshProcessDeath {
  uint8_t protocol_version{kMeshProtocolVersion};
  Address dead_machined;

  void Serialize(BinaryWriter& w) const {
    w.Write<uint8_t>(static_cast<uint8_t>(MeshMessageType::kProcessDeath));
    w.Write<uint8_t>(protocol_version);
    w.Write<uint32_t>(dead_machined.Ip());
    w.Write<uint16_t>(dead_machined.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<MeshProcessDeath> {
    auto type = r.Read<uint8_t>();
    if (!type) return type.Error();
    if (static_cast<MeshMessageType>(*type) != MeshMessageType::kProcessDeath) {
      return Error{ErrorCode::kInvalidArgument, "MeshProcessDeath: unexpected message type"};
    }
    auto ver = r.Read<uint8_t>();
    if (!ver) return ver.Error();
    if (*ver != kMeshProtocolVersion) {
      return Error{ErrorCode::kInvalidArgument, "MeshProcessDeath: incompatible protocol version"};
    }
    MeshProcessDeath msg;
    msg.protocol_version = *ver;
    auto ip = r.Read<uint32_t>();
    if (!ip) return ip.Error();
    auto port = r.Read<uint16_t>();
    if (!port) return port.Error();
    msg.dead_machined = Address(*ip, *port);
    return msg;
  }
};

// Reads the leading tag without consuming it, so a receiver can dispatch a
// datagram to the matching message before deserializing the body.
[[nodiscard]] inline auto PeekMeshType(BinaryReader& r) -> Result<MeshMessageType> {
  auto b = r.Peek();
  if (!b) return b.Error();
  return static_cast<MeshMessageType>(std::to_integer<uint8_t>(*b));
}

}  // namespace atlas::machined

#endif  // ATLAS_LIB_NETWORK_MESH_GOSSIP_H_
