#ifndef ATLAS_LIB_SERVER_UPDATABLE_H_
#define ATLAS_LIB_SERVER_UPDATABLE_H_

#include <cstddef>
#include <vector>

namespace atlas {

class Updatable {
 public:
  virtual ~Updatable() = default;
  virtual void Update() = 0;

 private:
  friend class Updatables;
  int removal_handle_ = -1;  // index in Updatables::objects_, -1 = not registered
};

class Updatables {
 public:
  explicit Updatables(int num_levels = 2);

  Updatables(const Updatables&) = delete;
  Updatables& operator=(const Updatables&) = delete;

  auto Add(Updatable* object, int level = 0) -> bool;

  auto Remove(Updatable* object) -> bool;

  void Call();

  [[nodiscard]] auto size() const -> std::size_t { return size_; }
  // level_offsets_ has num_levels+1 entries (sentinel at end), so subtract 1.
  [[nodiscard]] auto NumLevels() const -> int {
    return static_cast<int>(level_offsets_.size()) - 1;
  }

 private:
  void Compact();  // remove nulled slots after call()

  std::vector<Updatable*> objects_;

  std::vector<int> level_offsets_;

  // Additions made during Call() are merged after the current iteration.
  struct PendingAdd {
    Updatable* object;
    int level;
  };
  std::vector<PendingAdd> pending_add_;

  std::size_t size_{0};  // logical count (excludes null slots)
  bool in_update_{false};
  int null_count_{0};  // number of null slots to clean up
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_UPDATABLE_H_
