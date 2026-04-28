#ifndef ATLAS_SERVER_BASEAPP_ID_CLIENT_H_
#define ATLAS_SERVER_BASEAPP_ID_CLIENT_H_

#include <cstdint>
#include <deque>

#include "server/entity_types.h"

namespace atlas {

// ============================================================================
// IDClient — water-level EntityID cache (lives in BaseApp)
//
// Manages a local pool of pre-allocated EntityIDs obtained from DBApp.
// Uses water-level thresholds to trigger asynchronous refill requests:
//
//   critically_low (16)    — allocate_id() returns kInvalidEntityID
//   low            (1024)  — trigger refill request to DBApp
//   desired        (4096)  — number of IDs to request per refill
//   high           (16384) — stop requesting more IDs
//
// Thresholds were sized to absorb startup login bursts at 500+ clients
// without falling into the dbapp-roundtrip gap on the first tick (a 200-
// to 300-ms window during which allocation would otherwise refuse).
// EntityID is uint32_t (4 B IDs); a 16k-cache leaked per crash is
// negligible against the address space.
//
// Thread safety: single-threaded (called from BaseApp main loop only).
// ============================================================================

class IDClient {
 public:
  // Watermark constants — public so tests can reference them by name
  // instead of hard-coding the numeric values, which would silently rot
  // any time the thresholds are retuned for a different load shape.
  static constexpr uint64_t kCriticallyLow = 16;
  static constexpr uint64_t kLow = 1024;
  static constexpr uint64_t kDesired = 4096;
  static constexpr uint64_t kHigh = 16384;

  IDClient() = default;

  // Allocate a single EntityID from the local cache.
  // Returns kInvalidEntityID if the cache is critically low or empty.
  [[nodiscard]] auto AllocateId() -> EntityID;

  // Feed a new range of IDs received from DBApp.
  void AddIds(EntityID start, EntityID end);

  // Returns true if available IDs are below the low watermark.
  [[nodiscard]] auto NeedsRefill() const -> bool;

  // How many IDs to request from DBApp (0 if above high watermark).
  [[nodiscard]] auto IdsToRequest() const -> uint32_t;

  // Current number of available IDs in the cache.
  [[nodiscard]] auto Available() const -> uint64_t { return total_available_; }

  // Returns true if available IDs are below the critically-low threshold.
  [[nodiscard]] auto IsCriticallyLow() const -> bool { return total_available_ < kCriticallyLow; }

 private:
  struct Range {
    EntityID start;
    EntityID end;
  };

  std::deque<Range> ranges_;
  uint64_t total_available_{0};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_ID_CLIENT_H_
