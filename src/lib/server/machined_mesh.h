#ifndef ATLAS_LIB_SERVER_MACHINED_MESH_H_
#define ATLAS_LIB_SERVER_MACHINED_MESH_H_

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "network/address.h"

namespace atlas {

// BigWorld-style decentralized machined membership (server/tools/bwmachined,
// cluster.cpp): each host's machined learns its peers from periodic broadcast
// HELLOs and sorts all members into a ring by address. A machined monitors only
// its ring successor (its "buddy"); when the buddy stops sending HELLOs it owns
// announcing that host's death, so a single failure yields one announcement
// rather than N. The address order is identical on every node, so all nodes
// agree on the same ring without coordination.
class MachinedMesh {
 public:
  static constexpr Duration kDefaultPeerTimeout =
      std::chrono::duration_cast<Duration>(std::chrono::seconds(6));

  // A restarted peer reuses its address with a fresh incarnation; the caller
  // resyncs registry state toward kNew and kRestarted peers.
  enum class Observation : uint8_t { kKnown, kNew, kRestarted };

  explicit MachinedMesh(const Address& self) : self_(self) {}

  void SetPeerTimeout(Duration timeout) { peer_timeout_ = timeout; }

  auto RecordHeartbeat(const Address& peer, uint64_t incarnation, TimePoint now) -> Observation {
    if (peer == self_) return Observation::kKnown;
    auto it = peers_.find(peer);
    if (it == peers_.end()) {
      peers_.emplace(peer, Peer{now, incarnation});
      return Observation::kNew;
    }
    const bool restarted = it->second.incarnation != incarnation;
    it->second.last_seen = now;
    it->second.incarnation = incarnation;
    return restarted ? Observation::kRestarted : Observation::kKnown;
  }

  // Successor of self in the address-sorted ring of {self} + known peers, i.e.
  // the peer this machined monitors. nullopt when self is the only member.
  [[nodiscard]] auto Buddy() const -> std::optional<Address> {
    const auto ring = Ring();
    if (ring.size() < 2) return std::nullopt;
    for (std::size_t i = 0; i < ring.size(); ++i) {
      if (ring[i] == self_) return ring[(i + 1) % ring.size()];
    }
    return std::nullopt;
  }

  struct FailureScan {
    std::vector<Address> owned;   // deaths self announces cluster-wide
    std::vector<Address> pruned;  // every peer dropped this scan (owned is a subset)
  };

  // Per-tick failure scan. `owned` is the contiguous run of timed-out peers
  // clockwise of self (the deaths self broadcasts, one announcer per death);
  // `pruned` is every timed-out peer, dropped so a missed death broadcast cannot
  // linger and so a non-announcer can still evict the dead host's cached state.
  auto ScanFailures(TimePoint now) -> FailureScan {
    FailureScan result;
    const auto ring = Ring();
    const std::size_t n = ring.size();
    if (n > 1) {
      std::size_t self_idx = 0;
      for (std::size_t i = 0; i < n; ++i) {
        if (ring[i] == self_) {
          self_idx = i;
          break;
        }
      }
      for (std::size_t step = 1; step < n; ++step) {
        const Address& cand = ring[(self_idx + step) % n];
        auto it = peers_.find(cand);
        if (it == peers_.end()) continue;
        if (now - it->second.last_seen >= peer_timeout_) {
          result.owned.push_back(cand);
        } else {
          break;
        }
      }
    }
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (now - it->second.last_seen >= peer_timeout_) {
        result.pruned.push_back(it->first);
        it = peers_.erase(it);
      } else {
        it = std::next(it);
      }
    }
    return result;
  }

  [[nodiscard]] auto KnownPeerCount() const -> std::size_t { return peers_.size(); }

  [[nodiscard]] auto Contains(const Address& peer) const -> bool {
    return peers_.find(peer) != peers_.end();
  }

  // Address-sorted ring of {self} + known peers; identical order on every node.
  [[nodiscard]] auto Ring() const -> std::vector<Address> {
    std::vector<Address> ring;
    ring.reserve(peers_.size() + 1);
    ring.push_back(self_);
    for (const auto& [addr, _] : peers_) ring.push_back(addr);
    std::sort(ring.begin(), ring.end());
    return ring;
  }

 private:
  struct Peer {
    TimePoint last_seen{};
    uint64_t incarnation{0};
  };

  Address self_;
  std::unordered_map<Address, Peer> peers_;
  Duration peer_timeout_{kDefaultPeerTimeout};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MACHINED_MESH_H_
