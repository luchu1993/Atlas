#ifndef ATLAS_LIB_SERVER_MESH_REGISTRY_H_
#define ATLAS_LIB_SERVER_MESH_REGISTRY_H_

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

#include "foundation/process_type.h"
#include "network/address.h"
#include "network/machined_types.h"

namespace atlas {

// Cache of processes owned by *other* hosts' machined daemons, learned from mesh
// gossip. Keyed by the owning machined's mesh address so a peer's death drops
// exactly its processes. Remote entries carry no local channel (they cannot be
// shut down from here), so they live apart from the local ProcessRegistry; a
// query merges the two.
class MeshRegistry {
 public:
  // Replaces every process owned by `owner`; an empty set drops the owner (a
  // peer reporting nothing has effectively left).
  void UpdateOwner(const Address& owner, std::vector<machined::ProcessInfo> processes) {
    if (processes.empty()) {
      by_owner_.erase(owner);
      return;
    }
    by_owner_.insert_or_assign(owner, std::move(processes));
  }

  // Drops everything owned by `owner` (the peer machined died or left the mesh).
  auto DropOwner(const Address& owner) -> bool { return by_owner_.erase(owner) != 0; }

  // Removes and returns `owner`'s processes (empty if none), so a caller can
  // notify listeners of a dead host's processes as they are evicted.
  auto TakeOwner(const Address& owner) -> std::vector<machined::ProcessInfo> {
    auto it = by_owner_.find(owner);
    if (it == by_owner_.end()) return {};
    auto out = std::move(it->second);
    by_owner_.erase(it);
    return out;
  }

  // Every known remote process of `type`, across all owners.
  [[nodiscard]] auto FindByType(ProcessType type) const -> std::vector<machined::ProcessInfo> {
    std::vector<machined::ProcessInfo> out;
    for (const auto& [owner, procs] : by_owner_) {
      for (const auto& p : procs) {
        if (p.process_type == type) out.push_back(p);
      }
    }
    return out;
  }

  [[nodiscard]] auto HasOwner(const Address& owner) const -> bool {
    return by_owner_.find(owner) != by_owner_.end();
  }
  [[nodiscard]] auto OwnerCount() const -> std::size_t { return by_owner_.size(); }
  [[nodiscard]] auto ProcessCount() const -> std::size_t {
    std::size_t total = 0;
    for (const auto& [owner, procs] : by_owner_) total += procs.size();
    return total;
  }

 private:
  std::unordered_map<Address, std::vector<machined::ProcessInfo>> by_owner_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MESH_REGISTRY_H_
