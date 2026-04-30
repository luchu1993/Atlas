#ifndef ATLAS_LIB_SPACE_CONTROLLER_H_
#define ATLAS_LIB_SPACE_CONTROLLER_H_

#include <cstdint>

namespace atlas {

class Controllers;
class IEntityMotion;

using ControllerID = uint32_t;

enum class ControllerKind : uint8_t {
  kUnknown = 0,
  kMoveToPoint = 1,
  kTimer = 2,
  kProximity = 3,
};

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

  virtual void Start() {}
  virtual void Update(float dt) = 0;
  virtual void Stop() {}

  [[nodiscard]] virtual auto TypeTag() const -> ControllerKind { return ControllerKind::kUnknown; }

 protected:
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
