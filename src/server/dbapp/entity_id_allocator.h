#ifndef ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
#define ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_

#include <cstdint>
#include <functional>

#include "db/idatabase.h"
#include "foundation/log.h"
#include "server/entity_types.h"

namespace atlas {

// ============================================================================
// EntityIdAllocator — authoritative EntityID generator (lives in DBApp)
//
// Allocates monotonically increasing EntityIDs.  The current counter is
// periodically persisted to the database with a safety buffer so that a
// crash-restart never reuses IDs that were handed out but not yet persisted.
//
// Thread safety: single-threaded (called from DBApp main loop only).
// ============================================================================

class EntityIdAllocator {
 public:
  explicit EntityIdAllocator(IDatabase& db);

  // Load the counter from the database.  Must be called once at startup
  // before any allocate() call.  The callback receives true on success.
  void Startup(std::function<void(bool)> callback);

  // Allocate a contiguous range of `count` IDs.
  // Returns {start, end} where end = start + count - 1.
  // start is always >= 1 (kInvalidEntityID == 0 is never allocated).
  [[nodiscard]] auto Allocate(uint32_t count) -> std::pair<EntityID, EntityID>;

  // Persist the counter to the database if it has advanced past the
  // previously persisted watermark.
  void PersistIfNeeded(std::function<void(bool)> callback);

  // Force-persist the counter (e.g. on graceful shutdown).
  void Persist(std::function<void(bool)> callback);

  [[nodiscard]] auto next_id() const -> EntityID { return next_id_; }

 private:
  IDatabase& db_;
  EntityID next_id_{1};
  EntityID persisted_up_to_{0};

  // After a crash, the allocator restarts from persisted_up_to_, which is
  // always >= next_id_ + kSafetyBuffer at the time of the last persist.
  // This ensures any IDs handed out between the last persist and the crash
  // are skipped on recovery.
  static constexpr EntityID kSafetyBuffer = 100'000;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
