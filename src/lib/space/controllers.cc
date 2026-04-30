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
  raw->Start();
  return id;
}

auto Controllers::AddWithPreservedId(std::unique_ptr<Controller> ctrl, IEntityMotion* motion,
                                     int user_arg, ControllerID preserved_id) -> ControllerID {
  if (preserved_id == 0 || controllers_.contains(preserved_id)) return 0;
  ctrl->id_ = preserved_id;
  ctrl->motion_ = motion;
  ctrl->user_arg_ = user_arg;
  ctrl->finished_ = false;
  auto* raw = ctrl.get();
  controllers_.emplace(preserved_id, std::move(ctrl));
  if (preserved_id >= next_id_) next_id_ = preserved_id + 1;
  raw->Start();
  return preserved_id;
}

auto Controllers::Cancel(ControllerID id) -> bool {
  auto it = controllers_.find(id);
  if (it == controllers_.end()) return false;

  if (in_update_) {
    pending_cancel_.push_back(id);
    return true;
  }

  it->second->Stop();
  controllers_.erase(it);
  return true;
}

void Controllers::Update(float dt) {
  if (controllers_.empty()) return;

  in_update_ = true;

  tick_keys_.clear();
  tick_keys_.reserve(controllers_.size());
  for (auto& [id, _] : controllers_) tick_keys_.push_back(id);

  for (auto id : tick_keys_) {
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
  assert(!in_update_ && "Controllers::StopAll called during Update; use Cancel() for defer");

  Compact();
  for (auto& [_, ctrl] : controllers_) ctrl->Stop();
  controllers_.clear();
  pending_cancel_.clear();
}

void Controllers::Compact() {
  for (auto id : pending_cancel_) {
    auto it = controllers_.find(id);
    if (it == controllers_.end()) continue;
    it->second->Stop();
    controllers_.erase(it);
  }
  pending_cancel_.clear();

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
