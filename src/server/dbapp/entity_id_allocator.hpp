#pragma once

#include "db/idatabase.hpp"
#include "foundation/log.hpp"
#include "server/entity_types.hpp"

#include <cstdint>
#include <functional>

namespace atlas
{

// ============================================================================
// EntityIdAllocator — authoritative EntityID generator (lives in DBApp)
//
// Allocates monotonically increasing EntityIDs.  The current counter is
// periodically persisted to the database with a safety buffer so that a
// crash-restart never reuses IDs that were handed out but not yet persisted.
//
// Thread safety: single-threaded (called from DBApp main loop only).
// ============================================================================

class EntityIdAllocator
{
public:
    explicit EntityIdAllocator(IDatabase& db);

    // Load the counter from the database.  Must be called once at startup
    // before any allocate() call.  The callback receives true on success.
    void startup(std::function<void(bool)> callback);

    // Allocate a contiguous range of `count` IDs.
    // Returns {start, end} where end = start + count - 1.
    // start is always >= 1 (kInvalidEntityID == 0 is never allocated).
    [[nodiscard]] auto allocate(uint32_t count) -> std::pair<EntityID, EntityID>;

    // Persist the counter to the database if it has advanced past the
    // previously persisted watermark.
    void persist_if_needed(std::function<void(bool)> callback);

    // Force-persist the counter (e.g. on graceful shutdown).
    void persist(std::function<void(bool)> callback);

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
