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

// Per-tick Ghost membership computation. For each Real, finds peer
// CellApps overlapped by `pos +/- (ghost_distance + hysteresis)` and diffs
// against the current Haunt list to produce create/delete ops. Pure;
// CellApp does dispatch. Hysteresis dampens border flapping;
// min_ghost_lifespan prevents bounce on quick re-entry.
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

  // Maps peer addr -> Channel*, isolating GhostMaintainer from CellApp's
  // private connection map.
  using ChannelResolver = std::function<Channel*(const Address&)>;

  GhostMaintainer(const Config& config, Address self_addr, ChannelResolver resolver);

  // `now` is injected so tests drive min-lifespan deterministically.
  auto Run(Space& space, TimePoint now) -> PendingWork;

 private:
  Config config_;
  Address self_addr_;
  ChannelResolver resolver_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_GHOST_MAINTAINER_H_
