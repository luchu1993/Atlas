#pragma once

#include "network/channel.hpp"
#include "network/seq_num.hpp"
#include "network/socket.hpp"
#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace atlas
{

// Reliable UDP wire format flags
namespace rudp
{
    inline constexpr uint8_t kFlagReliable = 0x01;
    inline constexpr uint8_t kFlagHasSeq   = 0x02;
    inline constexpr uint8_t kFlagHasAck   = 0x04;
    inline constexpr std::size_t kMaxUdpPayload = 1472 - 13;  // MTU - max header overhead
}

class ReliableUdpChannel : public Channel
{
public:
    ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                       Socket& shared_socket, const Address& remote);
    ~ReliableUdpChannel() override;

    [[nodiscard]] auto fd() const -> FdHandle override { return shared_socket_.fd(); }

    // Send the current bundle reliably (ACK required, retransmit on loss)
    [[nodiscard]] auto send_reliable() -> Result<void>;

    // Send the current bundle unreliably (no ACK, no retransmit)
    [[nodiscard]] auto send_unreliable() -> Result<void>;

    // Called by NetworkInterface when a datagram arrives from this peer
    void on_datagram_received(std::span<const std::byte> data);

    // Configuration
    void set_nodelay(bool enable) { nodelay_ = enable; }
    [[nodiscard]] auto nodelay() const -> bool { return nodelay_; }

    // Fast retransmit threshold: retransmit after this many skip-ACKs (0 = disabled)
    void set_fast_resend_thresh(uint32_t thresh) { fast_resend_thresh_ = thresh; }

    // Stats
    [[nodiscard]] auto rtt() const -> Duration { return rtt_; }
    [[nodiscard]] auto unacked_count() const -> uint32_t
    {
        return static_cast<uint32_t>(unacked_.size());
    }

protected:
    auto do_send(std::span<const std::byte> data) -> Result<size_t> override;

private:
    struct UnackedPacket
    {
        std::vector<std::byte> data;
        TimePoint sent_at;
        uint32_t send_count{1};
        uint32_t skip_count{0};   // fast retransmit: incremented when later packets are ACK'd
    };

    // Build packet header and prepend to payload
    auto build_packet(uint8_t flags, SeqNum seq, std::span<const std::byte> payload)
        -> std::vector<std::byte>;

    // ACK processing
    void process_ack(SeqNum ack_num, uint32_t ack_bits);
    void update_rtt(Duration sample);

    // Resend logic
    void check_resends();
    void start_resend_timer();
    void stop_resend_timer();

    // Receive tracking
    void record_received_seq(SeqNum seq);
    auto is_duplicate(SeqNum seq) const -> bool;

    Socket& shared_socket_;

    // Sending state
    SeqNum next_send_seq_{1};
    std::map<SeqNum, UnackedPacket> unacked_;
    uint32_t send_window_{256};

    // Receiving state
    SeqNum remote_seq_{0};       // highest received seq from peer
    uint32_t recv_ack_bits_{0};  // bitmask of received before remote_seq_

    // RTT estimation (Jacobson/Karels per RFC 6298)
    Duration rtt_{Milliseconds(200)};
    Duration rtt_var_{Milliseconds(100)};
    Duration rto_{std::chrono::seconds(1)};

    // KCP-inspired optimizations
    bool nodelay_{false};                 // true: 1.5x backoff, 30ms min RTO
    uint32_t fast_resend_thresh_{2};      // fast retransmit after N skip-ACKs (0=disabled)

    // Resend timer
    TimerHandle resend_timer_;
};

} // namespace atlas
