#include "space/controllers.h"

#include <cassert>
#include <utility>

namespace atlas {

auto Controllers::Add(std::unique_ptr<Controller> ctrl, IEntityMotion* motion, int user_arg)
    -> ControllerID {
  const auto id = next_id_++;
  ctrl->id_ = id;
  ctrl->motion_ = motion;
  ctrl->user_arg_ = user_arg;
  ctrl->finished_ = false;
  auto* raw = ctrl.get();
  controllers_.emplace(id, std::move(ctrl));
  // Start() may synchronously call Finish() — that's fine; the next
  // Compact() pass will reap it.
  raw->Start();
  return id;
}

auto Controllers::Cancel(ControllerID id) -> bool {
  auto it = controllers_.find(id);
  if (it == controllers_.end()) return false;

  if (in_update_) {
    // Defer: erasing during iteration would invalidate the map's range.
    // Duplicate entries in pending_cancel_ are harmless — Compact()
    // treats them idempotently.
    pending_cancel_.push_back(id);
    return true;
  }

  it->second->Stop();
  controllers_.erase(it);
  return true;
}

void Controllers::Update(float dt) {
  in_update_ = true;

  // Snapshot the keys we want to tick, so Add() during iteration doesn't
  // accidentally tick the newcomer this tick (deterministic frame
  // boundary). unordered_map iteration is in bucket order not insertion
  // order — good enough; Controllers don't rely on tick order.
  std::vector<ControllerID> tick_keys;
  tick_keys.reserve(controllers_.size());
  for (auto& [id, _] : controllers_) tick_keys.push_back(id);

  for (auto id : tick_keys) {
    auto it = controllers_.find(id);
    if (it == controllers_.end()) continue;  // cancelled mid-loop
    auto& ctrl = *it->second;
    if (ctrl.finished_) continue;
    ctrl.Update(dt);
  }

  in_update_ = false;
  Compact();
}

void Controllers::StopAll() {
  // Re-entering StopAll from inside a controller's Update — e.g. a
  // script callback that triggers entity destruction mid-tick — would
  // modify `controllers_` while Update is still iterating it and lead
  // to UB. The supported pattern for "cancel during tick" is Cancel(),
  // which defers the erase until Compact() at the end of the tick.
  assert(!in_update_ && "Controllers::StopAll called during Update; use Cancel() for defer");

  // Drain pending cancels first in case a previous Update left any
  // deferred items — we're about to destroy everything anyway, but
  // calling Stop() in consistent order makes shutdown debugging saner.
  Compact();
  for (auto& [_, ctrl] : controllers_) ctrl->Stop();
  controllers_.clear();
  pending_cancel_.clear();
}

void Controllers::Compact() {
  // 1) explicit cancels requested during Update
  for (auto id : pending_cancel_) {
    auto it = controllers_.find(id);
    if (it == controllers_.end()) continue;
    it->second->Stop();
    controllers_.erase(it);
  }
  pending_cancel_.clear();

  // 2) controllers that finished naturally during Update
  for (auto it = controllers_.begin(); it != controllers_.end();) {
    if (it->second->finished_) {
      it->second->Stop();
      it = controllers_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace atlas
