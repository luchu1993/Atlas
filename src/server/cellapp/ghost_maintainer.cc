#include "ghost_maintainer.h"

#include <unordered_map>
#include <utility>

#include "cell_entity.h"
#include "foundation/log.h"
#include "real_entity_data.h"
#include "space.h"

namespace atlas {

GhostMaintainer::GhostMaintainer(const Config& config, Address self_addr, ChannelResolver resolver)
    : config_(config), self_addr_(self_addr), resolver_(std::move(resolver)) {}

auto GhostMaintainer::Run(Space& space, TimePoint now) -> PendingWork {
  PendingWork work;
  const auto* bsp = space.GetBspTree();
  if (bsp == nullptr) return work;

  space.ForEachEntity([&](CellEntity& entity) {
    if (!entity.IsReal()) return;
    auto* real_data = entity.GetRealData();
    if (real_data == nullptr) return;

    const auto& pos = entity.Position();
    const float pad = config_.ghost_distance + config_.hysteresis;
    CellBounds interest{pos.x - pad, pos.z - pad, pos.x + pad, pos.z + pad};

    // Dedupe by addr: BSP may surface multiple cells on the same peer.
    std::unordered_map<Address, cellappmgr::CellID> peer_cells;
    bsp->VisitRect(interest, [&](const CellInfo& ci) {
      if (ci.cellapp_addr == self_addr_) return;
      peer_cells.emplace(ci.cellapp_addr, ci.cell_id);
    });

    // Creates: walk peer_cells (not haunts) so a peer that gained a cell
    // mid-run is ghosted on this pass.
    for (const auto& [peer_addr, peer_cell_id] : peer_cells) {
      auto* channel = resolver_ ? resolver_(peer_addr) : nullptr;
      if (channel == nullptr) {
        ATLAS_LOG_WARNING("GhostMaintainer: no channel for peer CellApp {}:{} — skipping create",
                          peer_addr.Ip(), peer_addr.Port());
        continue;
      }
      if (real_data->HasHaunt(channel)) continue;
      work.creates.push_back(CreateOp{&entity, peer_addr, peer_cell_id});
    }

    // Deletes match by Address, not Channel*: a peer reconnect changes
    // the Channel* identity but should not tear down a still-relevant Ghost.
    for (const auto& h : real_data->Haunts()) {
      if (h.channel == nullptr) continue;
      if (peer_cells.contains(h.addr)) continue;

      const auto age = now - h.creation_time;
      if (age < config_.min_ghost_lifespan) continue;

      work.deletes.push_back(DeleteOp{&entity, h.channel, h.addr});
    }
  });

  return work;
}

}  // namespace atlas
