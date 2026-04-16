#ifndef ATLAS_LIB_SERVER_UPDATABLE_H_
#define ATLAS_LIB_SERVER_UPDATABLE_H_

#include <cstddef>
#include <vector>

namespace atlas {

// ============================================================================
// Updatable — interface for objects that receive a tick callback each frame
// ============================================================================

class Updatable {
 public:
  virtual ~Updatable() = default;
  virtual void Update() = 0;

 private:
  friend class Updatables;
  int removal_handle_ = -1;  // index in Updatables::objects_, -1 = not registered
};

// ============================================================================
// Updatables — manages a collection of Updatable objects
//
// Storage layout: objects_ is a flat array partitioned by level.
//   [level-0 objects... | level-1 objects... | ...]
// level_offsets_[i] = start index of level i in objects_.
//
// Tick ordering: lower levels are called before higher levels.
//
// Reentrancy: remove() during call() is safe — the slot is nulled and
// cleaned up after all levels have been iterated.
// add() during call() is also safe — new objects are buffered and merged
// after call() completes.
// ============================================================================

class Updatables {
 public:
  explicit Updatables(int num_levels = 2);

  // Non-copyable
  Updatables(const Updatables&) = delete;
  Updatables& operator=(const Updatables&) = delete;

  /// Register object at the given level (0 = highest priority, called first).
  /// Returns false if already registered.
  auto Add(Updatable* object, int level = 0) -> bool;

  /// Deregister object. Safe to call from within update().
  /// Returns false if not registered.
  auto Remove(Updatable* object) -> bool;

  /// Invoke update() on all registered objects in level order.
  void Call();

  [[nodiscard]] auto size() const -> std::size_t { return size_; }
  // level_offsets_ has num_levels+1 entries (sentinel at end), so subtract 1.
  [[nodiscard]] auto NumLevels() const -> int {
    return static_cast<int>(level_offsets_.size()) - 1;
  }

 private:
  void Compact();  // remove nulled slots after call()

  // Flat array of object pointers, partitioned by level.
  // Null slots are removed after each call().
  std::vector<Updatable*> objects_;

  // level_offsets_[i] = first index of level i in objects_.
  // level_offsets_[num_levels] = objects_.size() (sentinel).
  std::vector<int> level_offsets_;

  // Pending additions collected during call() — flushed after call() completes.
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
