#pragma once

#include "server/entity_types.hpp"

#include <cstdint>
#include <deque>

namespace atlas
{

// ============================================================================
// IDClient — BigWorld-style water-level EntityID cache (lives in BaseApp)
//
// Manages a local pool of pre-allocated EntityIDs obtained from DBApp.
// Uses water-level thresholds to trigger asynchronous refill requests:
//
//   critically_low (5)   — allocate_id() returns kInvalidEntityID
//   low            (64)  — trigger refill request to DBApp
//   desired        (256) — number of IDs to request per refill
//   high           (1024)— stop requesting more IDs
//
// Thread safety: single-threaded (called from BaseApp main loop only).
// ============================================================================

class IDClient
{
public:
    IDClient() = default;

    // Allocate a single EntityID from the local cache.
    // Returns kInvalidEntityID if the cache is critically low or empty.
    [[nodiscard]] auto allocate_id() -> EntityID;

    // Feed a new range of IDs received from DBApp.
    void add_ids(EntityID start, EntityID end);

    // Returns true if available IDs are below the low watermark.
    [[nodiscard]] auto needs_refill() const -> bool;

    // How many IDs to request from DBApp (0 if above high watermark).
    [[nodiscard]] auto ids_to_request() const -> uint32_t;

    // Current number of available IDs in the cache.
    [[nodiscard]] auto available() const -> uint64_t { return total_available_; }

    // Returns true if available IDs are below the critically-low threshold.
    [[nodiscard]] auto is_critically_low() const -> bool
    {
        return total_available_ < kCriticallyLow;
    }

private:
    struct Range
    {
        EntityID start;
        EntityID end;
    };

    std::deque<Range> ranges_;
    uint64_t total_available_{0};

    static constexpr uint64_t kCriticallyLow = 5;
    static constexpr uint64_t kLow = 64;
    static constexpr uint64_t kDesired = 256;
    static constexpr uint64_t kHigh = 1024;
};

}  // namespace atlas
