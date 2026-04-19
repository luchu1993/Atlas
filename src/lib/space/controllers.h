#ifndef ATLAS_LIB_SPACE_CONTROLLERS_H_
#define ATLAS_LIB_SPACE_CONTROLLERS_H_

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "space/controller.h"

namespace atlas {

class IEntityMotion;

// ============================================================================
// Controllers — per-CellEntity container for active Controller instances
//
// The map is keyed by a stable ControllerID so callers can hold a handle
// across ticks without worrying about iterator invalidation (entities
// tick 10-30 Hz; adding/removing controllers mid-tick is routine).
//
// Reentrancy rules:
//   - Cancel(id) during Update() defers the erase until Update() returns,
//     avoiding modification of the map while we're iterating it.
//   - Add() during Update() is allowed (new controller begins ticking
//     next frame; we don't call its Update this tick to keep behaviour
//     deterministic).
//   - StopAll() is only safe outside Update() — it calls Stop on every
//     surviving controller and clears the map.
// ============================================================================

class Controllers {
 public:
  Controllers() = default;
  ~Controllers() = default;

  Controllers(const Controllers&) = delete;
  auto operator=(const Controllers&) -> Controllers& = delete;

  // Take ownership of `ctrl`, wire it to the motion surface, call Start(),
  // and store under a fresh ControllerID. `motion` may be nullptr for
  // pure time-based controllers (TimerController) that never touch the
  // entity's position.
  auto Add(std::unique_ptr<Controller> ctrl, IEntityMotion* motion, int user_arg) -> ControllerID;

  // Mark `id` for removal. If called during Update() the actual destruction
  // is deferred until the tick finishes; Stop() runs as part of removal.
  // Returns true if `id` matched a live controller (even if deferred).
  auto Cancel(ControllerID id) -> bool;

  // Tick every live controller. Reentrant-safe wrt Cancel() on active or
  // sibling controllers; any controller that transitions to Finished()
  // during Update is also reaped at the end of the same tick.
  void Update(float dt);

  // Stop + drop every controller. Must NOT be called during Update().
  void StopAll();

  [[nodiscard]] auto Count() const -> std::size_t { return controllers_.size(); }
  [[nodiscard]] auto Contains(ControllerID id) const -> bool {
    return controllers_.find(id) != controllers_.end();
  }

 private:
  // After Update returns, process deferred cancels and finished reaps.
  void Compact();

  std::unordered_map<ControllerID, std::unique_ptr<Controller>> controllers_;
  std::vector<ControllerID> pending_cancel_;
  ControllerID next_id_{1};
  bool in_update_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_CONTROLLERS_H_
