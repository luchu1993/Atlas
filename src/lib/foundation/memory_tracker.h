#ifndef ATLAS_LIB_FOUNDATION_MEMORY_TRACKER_H_
#define ATLAS_LIB_FOUNDATION_MEMORY_TRACKER_H_

#include <atomic>
#include <cstddef>

namespace atlas {

class MemoryTracker {
 public:
  static auto Instance() -> MemoryTracker&;

  struct Stats {
    std::size_t current_bytes{0};
    std::size_t peak_bytes{0};
    std::size_t total_allocations{0};
    std::size_t total_deallocations{0};
  };

  void RecordAlloc(std::size_t bytes);
  void RecordDealloc(std::size_t bytes);

  [[nodiscard]] auto GetStats() const -> Stats;
  void Reset();

 private:
  MemoryTracker() = default;
  std::atomic<std::size_t> current_bytes_{0};
  std::atomic<std::size_t> peak_bytes_{0};
  std::atomic<std::size_t> total_allocs_{0};
  std::atomic<std::size_t> total_deallocs_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_MEMORY_TRACKER_H_
