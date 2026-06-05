#ifndef ATLAS_LIB_SERVER_MACHINED_MESH_NODE_H_
#define ATLAS_LIB_SERVER_MACHINED_MESH_NODE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "foundation/clock.h"
#include "foundation/error.h"
#include "network/address.h"
#include "network/mesh_gossip.h"
#include "serialization/binary_stream.h"
#include "server/machined_mesh.h"
#include "server/mesh_transport.h"

namespace atlas {

class EventDispatcher;

// Runtime glue that drives the decentralized machined mesh: it owns the
// broadcast transport and the membership view, routes received mesh datagrams by
// type, re-broadcasts this host's HELLO on an interval, folds received HELLOs
// into the ring, and reports the dead buddies it owns announcing. Received
// HELLOs are queued and timestamped inside Tick(now) so all membership time
// advances from a single clock the caller controls. MachinedApp owns one of
// these and Tick()s it each tick.
class MachinedMeshNode {
 public:
  using DeathCallback = std::function<void(const Address& dead_machined)>;
  using PeerEventCallback =
      std::function<void(const Address& peer, MachinedMesh::Observation observation)>;

  static constexpr Duration kDefaultBroadcastInterval =
      std::chrono::duration_cast<Duration>(std::chrono::seconds(2));

  MachinedMeshNode(EventDispatcher& dispatcher, const Address& self_mesh_addr)
      : self_(self_mesh_addr), mesh_(self_mesh_addr), transport_(dispatcher) {
    transport_.SetDatagramCallback(
        [this](const Address& src, std::span<const std::byte> payload) {
          OnDatagram(src, payload);
        });
  }

  [[nodiscard]] auto Open(const Address& bind_addr, const Address& broadcast_addr,
                          uint64_t incarnation) -> Result<void> {
    incarnation_ = incarnation;
    return transport_.Open(bind_addr, broadcast_addr);
  }
  void Close() { transport_.Close(); }

  void SetBroadcastInterval(Duration interval) { broadcast_interval_ = interval; }
  void SetPeerTimeout(Duration timeout) { mesh_.SetPeerTimeout(timeout); }
  void SetBroadcastTarget(const Address& addr) { transport_.SetBroadcastTarget(addr); }
  void SetDeathCallback(DeathCallback cb) { death_cb_ = std::move(cb); }
  void SetPeerEventCallback(PeerEventCallback cb) { peer_cb_ = std::move(cb); }

  // Folds queued HELLOs into the ring, re-broadcasts on the interval, and emits
  // the death callback for each dead buddy this node owns announcing.
  void Tick(TimePoint now) {
    for (const auto& p : pending_) {
      const auto observation = mesh_.RecordHeartbeat(p.addr, p.incarnation, now);
      if (observation != MachinedMesh::Observation::kKnown && peer_cb_) {
        peer_cb_(p.addr, observation);
      }
    }
    pending_.clear();

    if (now - last_broadcast_ >= broadcast_interval_) {
      machined::MeshHello hello;
      hello.machined_addr = self_;
      hello.incarnation = incarnation_;
      BinaryWriter w;
      hello.Serialize(w);
      (void)transport_.Broadcast(w.Data());
      last_broadcast_ = now;
    }

    for (const auto& dead : mesh_.ScanFailures(now)) {
      if (death_cb_) death_cb_(dead);
    }
  }

  [[nodiscard]] auto IsOpen() const -> bool { return transport_.IsOpen(); }
  [[nodiscard]] auto BoundAddress() const -> Address { return transport_.LocalAddress(); }
  [[nodiscard]] auto Incarnation() const -> uint64_t { return incarnation_; }
  [[nodiscard]] auto PeerCount() const -> std::size_t { return mesh_.KnownPeerCount(); }
  [[nodiscard]] auto Buddy() const -> std::optional<Address> { return mesh_.Buddy(); }

 private:
  struct Pending {
    Address addr;
    uint64_t incarnation;
  };

  void OnDatagram(const Address& /*src*/, std::span<const std::byte> payload) {
    BinaryReader r(payload);
    auto type = machined::PeekMeshType(r);
    if (!type) return;
    if (*type == machined::MeshMessageType::kHello) {
      auto hello = machined::MeshHello::Deserialize(r);
      if (hello) pending_.push_back(Pending{hello->machined_addr, hello->incarnation});
    }
  }

  Address self_;
  MachinedMesh mesh_;
  MeshTransport transport_;
  uint64_t incarnation_{0};
  Duration broadcast_interval_{kDefaultBroadcastInterval};
  TimePoint last_broadcast_{};
  std::vector<Pending> pending_;
  DeathCallback death_cb_;
  PeerEventCallback peer_cb_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MACHINED_MESH_NODE_H_
