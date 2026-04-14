#pragma once

#include "network/message.hpp"
#include "server/entity_types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace atlas
{

class Channel;

// ============================================================================
// DeltaForwarder — per-client bandwidth-limited delta relay
//
// CellApp sends property deltas (ReplicatedDeltaFromCell) to BaseApp, which
// must forward them to each client.  When many entities change in one tick the
// aggregate byte volume can spike.  DeltaForwarder queues incoming deltas and
// flushes them to the client channel each tick, respecting a byte budget.
//
// Deltas that exceed the budget stay queued; their deferred_ticks counter
// increments each tick, boosting their priority so starvation cannot occur.
// When the same entity receives a new delta while a previous one is still
// queued, the old entry is replaced — the client only needs the latest state.
// ============================================================================

class DeltaForwarder
{
public:
    struct Stats
    {
        uint64_t bytes_sent{0};
        uint64_t bytes_deferred{0};
    };

    /// Enqueue or replace a delta for the given entity.
    void enqueue(EntityID entity_id, std::span<const std::byte> delta);

    /// Flush queued deltas to `client_ch` using reserved message ID,
    /// stopping when `budget_bytes` would be exceeded.
    /// Returns the number of bytes actually sent.
    auto flush(Channel& client_ch, uint32_t budget_bytes) -> uint32_t;

    /// Current queue depth (number of pending entities).
    [[nodiscard]] auto queue_depth() const -> std::size_t { return queue_.size(); }

    [[nodiscard]] auto stats() const -> const Stats& { return stats_; }

    /// Reserved client-facing message ID for delta updates.
    static constexpr MessageID kClientDeltaMessageId = static_cast<MessageID>(0xF001);

private:
    struct PendingDelta
    {
        EntityID entity_id{kInvalidEntityID};
        std::vector<std::byte> delta;
        uint32_t deferred_ticks{0};
    };

    std::vector<PendingDelta> queue_;
    Stats stats_;
};

}  // namespace atlas
