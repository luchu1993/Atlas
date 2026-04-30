#ifndef ATLAS_LIB_SPACE_CONTROLLERS_H_
#define ATLAS_LIB_SPACE_CONTROLLERS_H_

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "space/controller.h"

namespace atlas {

class IEntityMotion;

class Controllers {
 public:
  Controllers() = default;
  ~Controllers() = default;

  Controllers(const Controllers&) = delete;
  auto operator=(const Controllers&) -> Controllers& = delete;

  auto Add(std::unique_ptr<Controller> ctrl, IEntityMotion* motion, int user_arg) -> ControllerID;

  // Cancel during Update() defers destruction until Compact().
  auto Cancel(ControllerID id) -> bool;

  void Update(float dt);

  void StopAll();

  [[nodiscard]] auto Count() const -> std::size_t { return controllers_.size(); }
  [[nodiscard]] auto Contains(ControllerID id) const -> bool {
    return controllers_.find(id) != controllers_.end();
  }

  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& [_, ctrl] : controllers_) fn(*ctrl);
  }

  // Migration restore path: add a controller with a pre-assigned ID
  // (origin-side value) so C# references to the ControllerID survive an
  // Offload round-trip.
  auto AddWithPreservedId(std::unique_ptr<Controller> ctrl, IEntityMotion* motion, int user_arg,
                          ControllerID preserved_id) -> ControllerID;

 private:
  void Compact();

  std::unordered_map<ControllerID, std::unique_ptr<Controller>> controllers_;
  std::vector<ControllerID> pending_cancel_;
  std::vector<ControllerID> tick_keys_;
  ControllerID next_id_{1};
  bool in_update_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_CONTROLLERS_H_
