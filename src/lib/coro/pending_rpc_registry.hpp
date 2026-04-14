#pragma once

#include "foundation/error.hpp"
#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"
#include "network/event_dispatcher.hpp"
#include "network/message.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <unordered_map>

namespace atlas
{

// ============================================================================
// PendingRpcRegistry — maps (MessageID, request_id) to coroutine callbacks
// ============================================================================

class PendingRpcRegistry
{
public:
    explicit PendingRpcRegistry(EventDispatcher& dispatcher);
    ~PendingRpcRegistry();

    PendingRpcRegistry(const PendingRpcRegistry&) = delete;
    PendingRpcRegistry& operator=(const PendingRpcRegistry&) = delete;

    using ReplyCallback = std::function<void(std::span<const std::byte> payload)>;
    using ErrorCallback = std::function<void(Error error)>;

    struct PendingHandle
    {
        MessageID reply_id{0};
        uint32_t request_id{0};
        [[nodiscard]] auto is_valid() const -> bool { return reply_id != 0; }
    };

    // Register a pending RPC. Returns a handle for manual cancellation.
    auto register_pending(MessageID reply_id, uint32_t request_id, ReplyCallback on_reply,
                          ErrorCallback on_error, Duration timeout) -> PendingHandle;

    // Try to consume an inbound message. Returns true if matched.
    // Convention: request_id is the first uint32_t (LE) in payload.
    auto try_dispatch(MessageID id, std::span<const std::byte> payload) -> bool;

    // Cancel a specific pending entry.
    void cancel(PendingHandle handle);

    // Cancel all pending entries (process shutdown).
    void cancel_all();

    [[nodiscard]] auto pending_count() const -> size_t;

private:
    struct PendingKey
    {
        MessageID reply_id;
        uint32_t request_id;
        auto operator==(const PendingKey&) const -> bool = default;
    };

    struct PendingKeyHash
    {
        auto operator()(const PendingKey& k) const -> size_t
        {
            auto h1 = std::hash<uint16_t>{}(k.reply_id);
            auto h2 = std::hash<uint32_t>{}(k.request_id);
            return h1 ^ (h2 << 16);
        }
    };

    struct PendingEntry
    {
        ReplyCallback on_reply;
        ErrorCallback on_error;
        TimerHandle timeout_timer;
    };

    static auto extract_request_id(std::span<const std::byte> payload) -> uint32_t;

    std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
    EventDispatcher& dispatcher_;
};

}  // namespace atlas
