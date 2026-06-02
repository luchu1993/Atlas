#ifndef ATLAS_LIB_SERVER_MESH_TRANSPORT_H_
#define ATLAS_LIB_SERVER_MESH_TRANSPORT_H_

#include <functional>
#include <optional>
#include <utility>

#include "foundation/error.h"
#include "network/address.h"
#include "network/mesh_gossip.h"
#include "network/socket.h"

namespace atlas {

class EventDispatcher;

// Owns the broadcast UDP socket each host's machined gossips on: it sends
// MeshHello datagrams (broadcast or directed) and delivers received ones to a
// callback. Self-contained — it registers its own fd with the dispatcher rather
// than sharing NetworkInterface's socket, so the mesh stays decoupled from the
// per-process channel machinery.
class MeshTransport {
 public:
  using HelloCallback =
      std::function<void(const Address& src, const machined::MeshHello& hello)>;

  explicit MeshTransport(EventDispatcher& dispatcher);
  ~MeshTransport();

  MeshTransport(const MeshTransport&) = delete;
  MeshTransport& operator=(const MeshTransport&) = delete;

  // Binds the mesh port (SO_REUSEADDR + SO_BROADCAST) and starts receiving.
  [[nodiscard]] auto Open(const Address& bind_addr, const Address& broadcast_addr) -> Result<void>;
  void Close();

  void SetHelloCallback(HelloCallback cb) { hello_cb_ = std::move(cb); }

  // Retargets BroadcastHello after Open (e.g. once the directed peer or subnet
  // broadcast address is known).
  void SetBroadcastTarget(const Address& addr) { broadcast_addr_ = addr; }

  [[nodiscard]] auto BroadcastHello(const machined::MeshHello& hello) -> Result<std::size_t>;
  [[nodiscard]] auto SendHelloTo(const Address& dest, const machined::MeshHello& hello)
      -> Result<std::size_t>;

  [[nodiscard]] auto IsOpen() const -> bool { return socket_.has_value(); }
  [[nodiscard]] auto LocalAddress() const -> Address { return local_addr_; }

 private:
  void OnReadable();

  EventDispatcher& dispatcher_;
  std::optional<Socket> socket_;
  Address local_addr_;
  Address broadcast_addr_;
  HelloCallback hello_cb_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MESH_TRANSPORT_H_
