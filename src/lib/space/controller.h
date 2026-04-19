#ifndef ATLAS_LIB_SPACE_CONTROLLER_H_
#define ATLAS_LIB_SPACE_CONTROLLER_H_

#include <cstdint>

namespace atlas {

class Controllers;
class IEntityMotion;

using ControllerID = uint32_t;

// ============================================================================
// Controller — behaviour attached to a CellEntity
//
// Controllers are the "verbs" a cell entity can perform over time: move to
// a point, fire a timer, watch a proximity volume. Each tick the owner
// CellEntity invokes Controllers::Update, which fans out to live children.
//
// Lifecycle:
//   1) Caller constructs a concrete subclass (MoveToPointController etc.)
//      and hands ownership to Controllers::Add.
//   2) Controllers::Add calls Start() once, after attaching entity/user_arg.
//   3) Update(dt) fires each tick until the subclass calls Finish().
//   4) Controllers either cancels explicitly or the finished flag evicts
//      the controller at the next safe point.
//
// Safety during tick:
//   Update may mark other controllers for cancellation (e.g. a Timer
//   ending a MoveToPoint). Controllers::Cancel defers the actual erase
//   until after the current Update pass finishes, so mid-iteration
//   cancellation is safe.
// ============================================================================

class Controller {
 public:
  Controller() = default;
  virtual ~Controller() = default;

  Controller(const Controller&) = delete;
  auto operator=(const Controller&) -> Controller& = delete;

  [[nodiscard]] auto Id() const -> ControllerID { return id_; }
  [[nodiscard]] auto Motion() -> IEntityMotion* { return motion_; }
  [[nodiscard]] auto UserArg() const -> int { return user_arg_; }
  [[nodiscard]] auto IsFinished() const -> bool { return finished_; }

  // Subclass hooks. Start runs once after Controllers::Add populates the
  // back-pointers; Update fires every tick while not finished; Stop runs
  // exactly once on removal (either via Cancel or post-Finish eviction).
  virtual void Start() {}
  virtual void Update(float dt) = 0;
  virtual void Stop() {}

 protected:
  // Mark the controller as done. The owning Controllers container will
  // reap it after the current tick. Subclasses call this when their
  // business is complete (e.g. MoveToPoint reached its destination).
  void Finish() { finished_ = true; }

 private:
  friend class Controllers;

  ControllerID id_{0};
  IEntityMotion* motion_{nullptr};
  int user_arg_{0};
  bool finished_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_CONTROLLER_H_
