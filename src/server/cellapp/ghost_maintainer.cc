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
  if (bsp == nullptr) return work;  // No geometry yet → no ghost decisions.

  space.ForEachEntity([&](CellEntity& entity) {
    if (!entity.IsReal()) return;
    auto* real_data = entity.GetRealData();
    if (real_data == nullptr) return;  // Defensive; IsReal() implies non-null.

    // Interest rect = position padded by ghost_distance + hysteresis.
    // Hysteresis extends the keep-alive zone beyond the create zone so a
    // Real that exited the crisp ghost_distance boundary doesn't tear
    // down its ghosts until it's clearly gone.
    const auto& pos = entity.Position();
    const float pad = config_.ghost_distance + config_.hysteresis;
    CellBounds interest{pos.x - pad, pos.z - pad, pos.x + pad, pos.z + pad};

    // Gather all peer cells whose bounds overlap our interest rect.
    // The BSP may return multiple CellInfos living on the same CellApp
    // (if a peer happens to own two cells we both see); dedupe by addr.
    std::unordered_map<Address, cellappmgr::CellID> peer_cells;
    bsp->VisitRect(interest, [&](const CellInfo& ci) {
      if (ci.cellapp_addr == self_addr_) return;  // Don't ghost ourselves.
      peer_cells.emplace(ci.cellapp_addr, ci.cell_id);
    });

    // Pass 1: creates — any peer in interest without an existing Haunt.
    // Walk peer_cells rather than haunts so a peer that gained a cell
    // mid-run gets ghosted on this pass.
    for (const auto& [peer_addr, peer_cell_id] : peer_cells) {
      auto* channel = resolver_ ? resolver_(peer_addr) : nullptr;
      if (channel == nullptr) {
        ATLAS_LOG_WARNING("GhostMaintainer: no channel for peer CellApp {}:{} — skipping create",
                          peer_addr.Ip(), peer_addr.Port());
        continue;
      }
      if (real_data->HasHaunt(channel)) continue;  // Already ghosted there.
      work.creates.push_back(CreateOp{&entity, peer_addr, peer_cell_id});
    }

    // Pass 2: deletes — haunts whose peer is no longer in interest AND
    // whose age exceeds min_ghost_lifespan. Dropping before min-lifespan
    // invites flapping when an entity bounces across a border, so we
    // hold the Ghost even after it leaves our interest zone briefly.
    for (const auto& h : real_data->Haunts()) {
      // We don't have a direct reverse mapping from channel → address
      // in GhostMaintainer; instead, walk peer_cells comparing
      // `resolver_(addr) == h.channel` to decide if the Haunt's peer is
      // still in interest. This is O(peers) per haunt; peer counts are
      // single-digit in realistic Space topologies.
      bool still_in_interest = false;
      Address still_peer_addr;
      for (const auto& [peer_addr, _] : peer_cells) {
        if (resolver_ && resolver_(peer_addr) == h.channel) {
          still_in_interest = true;
          still_peer_addr = peer_addr;
          break;
        }
      }
      if (still_in_interest) continue;

      // Age gate — respect min_ghost_lifespan.
      const auto age = now - h.creation_time;
      if (age < config_.min_ghost_lifespan) continue;

      work.deletes.push_back(DeleteOp{&entity, h.channel, still_peer_addr});
    }
  });

  return work;
}

}  // namespace atlas
