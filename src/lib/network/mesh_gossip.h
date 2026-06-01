#ifndef ATLAS_LIB_NETWORK_MESH_GOSSIP_H_
#define ATLAS_LIB_NETWORK_MESH_GOSSIP_H_

#include <cstddef>
#include <cstdint>

#include "foundation/error.h"
#include "network/address.h"
#include "serialization/binary_stream.h"

namespace atlas::machined {

inline constexpr uint8_t kMeshProtocolVersion = 1;

// Raw broadcast datagrams between per-host machined daemons (not Channel
// messages); each is self-framed by a leading MeshMessageType tag.
enum class MeshMessageType : uint8_t {
  kHello = 1,
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

// Reads the leading tag without consuming it, so a receiver can dispatch a
// datagram to the matching message before deserializing the body.
[[nodiscard]] inline auto PeekMeshType(BinaryReader& r) -> Result<MeshMessageType> {
  auto b = r.Peek();
  if (!b) return b.Error();
  return static_cast<MeshMessageType>(std::to_integer<uint8_t>(*b));
}

}  // namespace atlas::machined

#endif  // ATLAS_LIB_NETWORK_MESH_GOSSIP_H_
