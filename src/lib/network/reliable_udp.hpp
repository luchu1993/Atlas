#pragma once

#include "foundation/time.hpp"
#include "foundation/timer_queue.hpp"
#include "network/channel.hpp"
#include "network/rtt_estimator.hpp"
#include "network/seq_num.hpp"
#include "network/socket.hpp"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace atlas
{

// Reliable UDP wire format flags
namespace rudp
{
inline constexpr uint8_t kFlagReliable = 0x01;
inline constexpr uint8_t kFlagHasSeq = 0x02;
inline constexpr uint8_t kFlagHasAck = 0x04;
inline constexpr uint8_t kFlagFragment = 0x08;  // has fragment header (4 bytes)

// Header overhead: flags(1) + seq(4) + ack(4) + ack_bits(4) + frag(4) = 17
inline constexpr std::size_t kMaxHeaderSize = 17;
inline constexpr std::size_t kMtu = 1472;
inline constexpr std::size_t kMaxUdpPayload = kMtu - kMaxHeaderSize;
inline constexpr std::size_t kMaxFragments = 255;
inline constexpr Duration kFragmentTimeout = std::chrono::seconds(30);
}  // namespace rudp

// Thread safety: NOT thread-safe. Used from EventDispatcher's thread only.
class ReliableUdpChannel : public Channel
{
public:
    ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
                       const Address& remote);
    ~ReliableUdpChannel() override;

    [[nodiscard]] auto fd() const -> FdHandle override { return shared_socket_.fd(); }

    // Send the current bundle reliably (ACK required, retransmit on loss)
    [[nodiscard]] auto send_reliable() -> Result<void>;

    // Send the current bundle unreliably (no ACK, no retransmit)
    [[nodiscard]] auto send_unreliable() -> Result<void>;

    // Called by NetworkInterface when a datagram arrives from this peer
    void on_datagram_received(std::span<const std::byte> data);

    // Configuration
    void set_nodelay(bool enable) { rtt_.set_nodelay(enable); }
    [[nodiscard]] auto nodelay() const -> bool { return rtt_.nodelay(); }

    // Fast retransmit threshold: retransmit after this many skip-ACKs (0 = disabled)
    void set_fast_resend_thresh(uint32_t thresh) { fast_resend_thresh_ = thresh; }

    // Receive window size (max out-of-order buffered packets)
    void set_recv_window(uint32_t wnd) { rcv_wnd_ = wnd; }
    [[nodiscard]] auto recv_window() const -> uint32_t { return rcv_wnd_; }

    // Congestion control: disable to use fixed send_window_ (for LAN)
    void set_nocwnd(bool enable) { nocwnd_ = enable; }
    [[nodiscard]] auto nocwnd() const -> bool { return nocwnd_; }

    // Stats
    [[nodiscard]] auto rtt() const -> Duration { return rtt_.rtt(); }
    [[nodiscard]] auto unacked_count() const -> uint32_t
    {
        return static_cast<uint32_t>(unacked_.size());
    }
    [[nodiscard]] auto recv_next_seq() const -> SeqNum { return rcv_nxt_; }
    [[nodiscard]] auto recv_buf_count() const -> uint32_t
    {
        return static_cast<uint32_t>(rcv_buf_.size());
    }
    [[nodiscard]] auto cwnd() const -> uint32_t { return cwnd_; }
    [[nodiscard]] auto ssthresh() const -> uint32_t { return ssthresh_; }
    [[nodiscard]] auto effective_window() const -> uint32_t;

protected:
    [[nodiscard]] auto do_send(std::span<const std::byte> data) -> Result<size_t> override;

private:
    struct UnackedPacket
    {
        std::vector<std::byte> data;
        TimePoint sent_at;
        uint32_t send_count{1};
        uint32_t skip_count{0};  // fast retransmit: incremented when later packets are ACK'd
    };

    // Fragment header (4 bytes, present when kFlagFragment is set)
    struct FragmentHeader
    {
        uint16_t fragment_id;    // identifies the fragmented message
        uint8_t fragment_index;  // 0-based position
        uint8_t fragment_count;  // total fragments
    };

    struct FragmentGroup
    {
        uint8_t expected_count{0};
        uint8_t received_count{0};
        std::vector<std::byte> buffer;     // pre-allocated contiguous buffer
        std::vector<uint16_t> frag_sizes;  // per-fragment size (0 = not received)
        std::size_t total_size{0};
        TimePoint first_received;
    };

    // Build packet header and prepend to payload
    auto build_packet(uint8_t flags, SeqNum seq, std::span<const std::byte> payload,
                      const FragmentHeader* frag = nullptr) -> std::vector<std::byte>;

    // Rebuild packet header with fresh ACK info for retransmission
    auto rebuild_packet_header(const std::vector<std::byte>& original_packet)
        -> std::vector<std::byte>;

    // ACK processing
    void process_ack(SeqNum ack_num, uint32_t ack_bits);

    // Resend logic
    void check_resends();
    void start_resend_timer();
    void stop_resend_timer();

    // Independent ACK sending + delayed ACK
    void send_ack();
    void schedule_delayed_ack();
    void cancel_delayed_ack();

    // Receive tracking
    void record_received_seq(SeqNum seq);
    auto is_duplicate(SeqNum seq) const -> bool;

    // Fragmentation
    auto send_fragmented(std::span<const std::byte> payload) -> Result<void>;
    void on_fragment_received(const FragmentHeader& hdr, std::span<const std::byte> payload);
    void cleanup_stale_fragments();

    // Ordered delivery
    void enqueue_for_delivery(SeqNum seq, std::span<const std::byte> payload, bool is_fragment,
                              const FragmentHeader& frag_hdr);
    void flush_receive_buffer();

    // Congestion control
    void on_ack_cwnd_update(uint32_t acked_count);
    void on_loss_cwnd_update(bool is_timeout);

    Socket& shared_socket_;

    // Sending state
    SeqNum next_send_seq_{1};
    std::map<SeqNum, UnackedPacket> unacked_;
    uint32_t send_window_{256};

    // Receiving state — ACK tracking (tracks what we received, for telling sender)
    SeqNum remote_seq_{0};       // highest received seq from peer
    uint32_t recv_ack_bits_{0};  // bitmask of received before remote_seq_

    // Receiving state — ordered delivery (KCP-style rcv_buf → rcv_queue)
    struct RecvEntry
    {
        std::vector<std::byte> payload;
        bool is_fragment{false};
        FragmentHeader frag_hdr{};
    };
    std::map<SeqNum, RecvEntry> rcv_buf_;  // out-of-order receive buffer
    SeqNum rcv_nxt_{1};                    // next expected seq for ordered delivery
    uint32_t rcv_wnd_{256};                // receive window size

    // RTT estimation (Jacobson/Karels per RFC 6298) — extracted to RttEstimator
    RttEstimator rtt_;

    // KCP-inspired optimizations
    uint32_t fast_resend_thresh_{2};  // fast retransmit after N skip-ACKs (0=disabled)

    // Congestion control (KCP-style: slow start + congestion avoidance)
    bool nocwnd_{false};     // true: disable congestion control (for LAN)
    uint32_t cwnd_{1};       // congestion window (packets)
    uint32_t ssthresh_{16};  // slow start threshold
    uint32_t cwnd_incr_{0};  // byte-level increment for congestion avoidance

    // Fragmentation state
    uint16_t next_fragment_id_{1};
    std::unordered_map<uint16_t, FragmentGroup> pending_fragments_;
    static constexpr std::size_t kMaxPendingFragmentGroups = 64;

    // Resend timer
    TimerHandle resend_timer_;

    // Delayed ACK timer
    TimerHandle delayed_ack_timer_;
    bool ack_pending_{false};
    static constexpr Duration kDelayedAckTimeout = std::chrono::milliseconds(25);
};

}  // namespace atlas
