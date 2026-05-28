#include "movement_command_store.h"

namespace atlas {
namespace {

[[nodiscard]] auto IsCommandTypeSafe(movement::MovementCommandType type) -> bool {
  switch (type) {
    case movement::MovementCommandType::kDash:
    case movement::MovementCommandType::kLaunch:
    case movement::MovementCommandType::kLaunchOther:
    case movement::MovementCommandType::kPull:
    case movement::MovementCommandType::kKnockback:
    case movement::MovementCommandType::kTeleport:
    case movement::MovementCommandType::kFollowEntity:
      return true;
  }
  return false;
}

[[nodiscard]] auto IsInputPolicySafe(movement::MovementCommandInputPolicy policy) -> bool {
  switch (policy) {
    case movement::MovementCommandInputPolicy::kSuppress:
    case movement::MovementCommandInputPolicy::kAllowTurn:
      return true;
    case movement::MovementCommandInputPolicy::kAllowFull:
      return false;
  }
  return false;
}

[[nodiscard]] auto IsCollisionPolicySafe(
    movement::MovementCommandCollisionPolicy policy) -> bool {
  switch (policy) {
    case movement::MovementCommandCollisionPolicy::kStop:
    case movement::MovementCommandCollisionPolicy::kContinue:
    case movement::MovementCommandCollisionPolicy::kEndSkill:
      return true;
  }
  return false;
}

[[nodiscard]] auto IsMovementCommandSafe(const movement::MovementCommand& command) -> bool {
  if (command.command_id == 0 || command.duration_ms == 0 ||
      command.elapsed_ms > command.duration_ms) {
    return false;
  }
  if (!movement::IsFinite(command.start_position) ||
      !movement::IsFinite(command.target_position)) {
    return false;
  }
  return IsCommandTypeSafe(command.type) && IsInputPolicySafe(command.input_policy) &&
         IsCollisionPolicySafe(command.collision_policy);
}

}  // namespace

auto MovementCommandStore::Set(EntityID entity_id,
                               const movement::MovementCommand& command) -> bool {
  if (entity_id == kInvalidEntityID || !IsMovementCommandSafe(command)) return false;
  commands_[entity_id] = command;
  return true;
}

auto MovementCommandStore::Find(EntityID entity_id) -> movement::MovementCommand* {
  auto it = commands_.find(entity_id);
  return it == commands_.end() ? nullptr : &it->second;
}

auto MovementCommandStore::Find(EntityID entity_id) const -> const movement::MovementCommand* {
  auto it = commands_.find(entity_id);
  return it == commands_.end() ? nullptr : &it->second;
}

void MovementCommandStore::Erase(EntityID entity_id) {
  commands_.erase(entity_id);
}

void MovementCommandStore::AppendEntityIds(std::vector<EntityID>& out) const {
  out.reserve(out.size() + commands_.size());
  for (const auto& entry : commands_) out.push_back(entry.first);
}

}  // namespace atlas
