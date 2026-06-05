#ifndef ATLAS_LIB_SERVER_MESH_TRANSPORT_H_
#define ATLAS_LIB_SERVER_MESH_TRANSPORT_H_

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <utility>

#include "foundation/error.h"
#include "network/address.h"
#include "network/socket.h"

namespace atlas {

class EventDispatcher;

// Owns the broadcast UDP socket each host's machined gossips on: it sends and
// receives raw mesh datagrams and hands received ones to a callback. The wire
// framing lives in mesh_gossip.h and the routing in MachinedMeshNode, so this
// stays a plain datagram pipe. Self-contained — it registers its own fd with
// the dispatcher rather than sharing NetworkInterface's socket, keeping the
// mesh decoupled from the per-process channel machinery.
class MeshTransport {
 public:
  using DatagramCallback =
      std::function<void(const Address& src, std::span<const std::byte> payload)>;

  explicit MeshTransport(EventDispatcher& dispatcher);
  ~MeshTransport();

  MeshTransport(const MeshTransport&) = delete;
  MeshTransport& operator=(const MeshTransport&) = delete;

  // Binds the mesh port (SO_REUSEADDR + SO_BROADCAST) and starts receiving.
  [[nodiscard]] auto Open(const Address& bind_addr, const Address& broadcast_addr) -> Result<void>;
  void Close();

  void SetDatagramCallback(DatagramCallback cb) { datagram_cb_ = std::move(cb); }

  // Retargets Broadcast after Open (e.g. once the directed peer or subnet
  // broadcast address is known).
  void SetBroadcastTarget(const Address& addr) { broadcast_addr_ = addr; }

  [[nodiscard]] auto Broadcast(std::span<const std::byte> payload) -> Result<std::size_t>;
  [[nodiscard]] auto SendTo(const Address& dest, std::span<const std::byte> payload)
      -> Result<std::size_t>;

  [[nodiscard]] auto IsOpen() const -> bool { return socket_.has_value(); }
  [[nodiscard]] auto LocalAddress() const -> Address { return local_addr_; }

 private:
  void OnReadable();

  EventDispatcher& dispatcher_;
  std::optional<Socket> socket_;
  Address local_addr_;
  Address broadcast_addr_;
  DatagramCallback datagram_cb_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MESH_TRANSPORT_H_
