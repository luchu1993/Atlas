#pragma once

#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"
#include "network/address.hpp"
#include "network/bundle.hpp"
#include "network/packet_filter.hpp"
#include "platform/io_poller.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace atlas
{

class EventDispatcher;
class InterfaceTable;

enum class ChannelState : uint8_t
{
    Created,
    Active,
    Condemned,
};

// Thread safety: NOT thread-safe. Used from EventDispatcher's thread only.
class Channel
{
public:
    Channel(EventDispatcher& dispatcher, InterfaceTable& table, const Address& remote);
    virtual ~Channel();

    // Non-copyable, non-movable
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // Sending
    [[nodiscard]] auto bundle() -> Bundle& { return bundle_; }
    [[nodiscard]] auto send() -> Result<void>;
    // Both overloads honour Msg::descriptor().reliability (or the registered
    // InterfaceTable entry for the raw-ID variant):
    //   Reliable   → send()            (ACK + retransmit on RUDP, always on TCP)
    //   Unreliable → send_unreliable() (best-effort on RUDP, natural path on TCP/UDP)
    [[nodiscard]] auto send_message(MessageID id, std::span<const std::byte> data) -> Result<void>;

    template <NetworkMessage Msg>
    void queue_message(const Msg& msg)
    {
        bundle_.add_message(msg);
    }

    template <NetworkMessage Msg>
    [[nodiscard]] auto send_message(const Msg& msg) -> Result<void>
    {
        bundle_.add_message(msg);
        if (Msg::descriptor().is_unreliable())
            return send_unreliable();
        return send();
    }

    // Best-effort send — subclasses override to use unreliable path when available.
    // Default implementation falls back to send() (TCP is always reliable).
    [[nodiscard]] virtual auto send_unreliable() -> Result<void>;

    // State
    [[nodiscard]] auto state() const -> ChannelState { return state_; }
    [[nodiscard]] auto remote_address() const -> const Address& { return remote_; }
    [[nodiscard]] auto is_connected() const -> bool { return state_ == ChannelState::Active; }

    void activate();
    void condemn();

    // Inactivity detection
    void set_inactivity_timeout(Duration timeout);
    void reset_inactivity_timer();

    // Statistics
    [[nodiscard]] auto bytes_sent() const -> uint64_t { return bytes_sent_; }
    [[nodiscard]] auto bytes_received() const -> uint64_t { return bytes_received_; }

    // Packet filter (compression, encryption, etc.)
    void set_packet_filter(PacketFilterPtr filter) { packet_filter_ = std::move(filter); }
    [[nodiscard]] auto packet_filter() const -> PacketFilter* { return packet_filter_.get(); }

    // Disconnect callback
    using DisconnectCallback = std::function<void(Channel&)>;
    void set_disconnect_callback(DisconnectCallback cb);

    // Access underlying fd for IOPoller registration
    [[nodiscard]] virtual auto fd() const -> FdHandle = 0;

protected:
    // Subclass hooks
    [[nodiscard]] virtual auto do_send(std::span<const std::byte> data) -> Result<size_t> = 0;
    void on_data_received(std::span<const std::byte> data);
    void on_disconnect();

    // Message parsing helper: parse messages from a complete frame
    void dispatch_messages(std::span<const std::byte> frame_data);

    EventDispatcher& dispatcher_;
    InterfaceTable& interface_table_;
    Address remote_;
    ChannelState state_{ChannelState::Created};
    Bundle bundle_;

    uint64_t bytes_sent_{0};
    uint64_t bytes_received_{0};

    PacketFilterPtr packet_filter_;
    DisconnectCallback disconnect_callback_;
    Duration inactivity_timeout_{};
    TimerHandle inactivity_timer_;
    TimePoint last_activity_{};

private:
    void check_inactivity();
    void start_inactivity_timer();
};

}  // namespace atlas
