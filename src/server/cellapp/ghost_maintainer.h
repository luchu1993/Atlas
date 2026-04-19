#ifndef ATLAS_SERVER_CELLAPP_GHOST_MAINTAINER_H_
#define ATLAS_SERVER_CELLAPP_GHOST_MAINTAINER_H_

#include <cstddef>
#include <functional>
#include <vector>

#include "cellappmgr/bsp_tree.h"
#include "foundation/clock.h"
#include "network/address.h"

namespace atlas {

class CellEntity;
class Channel;
class Space;

// ============================================================================
// GhostMaintainer — computes per-tick Ghost membership changes.
//
// Phase 11 §3.3. Pull-model maintainer: for each Real entity in a Space,
// compute the set of peer CellApps whose partition overlaps the entity's
// interest area (`pos ± (ghost_distance + hysteresis)`). Compared against
// the Real's current Haunt list, that gives two work queues:
//
//   CreateOp  — Haunt-less peers overlapped by interest → emit CreateGhost
//   DeleteOp  — Haunts no longer overlapped AND past min-lifespan → emit
//               DeleteGhost
//
// Execution is intentionally split from dispatch: Run() returns a
// PendingWork record, CellApp walks it and sends the messages. This
// keeps the maintainer free of network dependencies and unit-testable
// on fakes.
//
// Hysteresis prevents flapping when entities sit exactly on a Cell
// boundary — matches BigWorld's GHOST_FUDGE semantics.
// min_ghost_lifespan prevents tearing down a Ghost that was just
// created (same tick, or near-same tick), which would otherwise bounce
// if the Real re-enters the interest area right after crossing out.
// ============================================================================

class GhostMaintainer {
 public:
  struct Config {
    float ghost_distance{500.f};
    float hysteresis{20.f};
    Duration min_ghost_lifespan{std::chrono::seconds(2)};
  };

  struct CreateOp {
    CellEntity* entity;  // Real entity emitting the Ghost
    Address peer_addr;   // CellApp that should host the Ghost
    cellappmgr::CellID peer_cell_id{0};
  };

  struct DeleteOp {
    CellEntity* entity;     // Real entity whose Ghost should drop
    Channel* peer_channel;  // the Haunt's stored channel
    Address peer_addr;
  };

  struct PendingWork {
    std::vector<CreateOp> creates;
    std::vector<DeleteOp> deletes;
  };

  // Channel resolver: maps peer CellApp address → Channel* so CreateOp
  // can be routed without GhostMaintainer depending on CellApp's private
  // connection map.
  using ChannelResolver = std::function<Channel*(const Address&)>;

  GhostMaintainer(const Config& config, Address self_addr, ChannelResolver resolver);

  // Walk `space`'s Reals and compute pending work. `now` is passed in so
  // tests can drive deterministic min-lifespan checks without stepping
  // the real clock.
  auto Run(Space& space, TimePoint now) -> PendingWork;

 private:
  Config config_;
  Address self_addr_;
  ChannelResolver resolver_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_GHOST_MAINTAINER_H_
