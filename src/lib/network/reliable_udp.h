#ifndef ATLAS_LIB_NETWORK_RELIABLE_UDP_H_
#define ATLAS_LIB_NETWORK_RELIABLE_UDP_H_

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "foundation/timer_queue.h"
#include "network/channel.h"
#include "network/rtt_estimator.h"
#include "network/seq_num.h"
#include "network/socket.h"

namespace atlas {

// Reliable UDP wire format flags
namespace rudp {
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
class ReliableUdpChannel : public Channel {
 public:
  ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
                     const Address& remote);
  ~ReliableUdpChannel() override;

  [[nodiscard]] auto Fd() const -> FdHandle override { return shared_socket_.Fd(); }

  // Send the current bundle reliably (ACK required, retransmit on loss)
  [[nodiscard]] auto SendReliable() -> Result<void>;

  // Send the current bundle unreliably (no ACK, no retransmit).
  // Overrides Channel::send_unreliable() so that typed messages whose
  // descriptor().reliability == Unreliable automatically use this path.
  [[nodiscard]] auto SendUnreliable() -> Result<void> override;

  // Called by NetworkInterface when a datagram arrives from this peer
  void OnDatagramReceived(std::span<const std::byte> data);

  // Configuration
  void SetNodelay(bool enable) { rtt_.SetNodelay(enable); }
  [[nodiscard]] auto Nodelay() const -> bool { return rtt_.Nodelay(); }

  // Fast retransmit threshold: retransmit after this many skip-ACKs (0 = disabled)
  void SetFastResendThresh(uint32_t thresh) { fast_resend_thresh_ = thresh; }

  // Receive window size (max out-of-order buffered packets)
  void SetSendWindow(uint32_t wnd) { send_window_ = wnd; }
  void SetRecvWindow(uint32_t wnd) { rcv_wnd_ = wnd; }
  [[nodiscard]] auto RecvWindow() const -> uint32_t { return rcv_wnd_; }

  // Congestion control: disable to use fixed send_window_ (for LAN)
  void SetNocwnd(bool enable) { nocwnd_ = enable; }
  [[nodiscard]] auto Nocwnd() const -> bool { return nocwnd_; }

  // Stats
  [[nodiscard]] auto Rtt() const -> Duration { return rtt_.Rtt(); }
  [[nodiscard]] auto UnackedCount() const -> uint32_t {
    return static_cast<uint32_t>(unacked_.size());
  }
  [[nodiscard]] auto RecvNextSeq() const -> SeqNum { return rcv_nxt_; }
  [[nodiscard]] auto RecvBufCount() const -> uint32_t {
    return static_cast<uint32_t>(rcv_buf_.size());
  }
  [[nodiscard]] auto Cwnd() const -> uint32_t { return cwnd_; }
  [[nodiscard]] auto Ssthresh() const -> uint32_t { return ssthresh_; }
  [[nodiscard]] auto EffectiveWindow() const -> uint32_t;

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;
  void OnCondemned() override;

 private:
  struct UnackedPacket {
    std::vector<std::byte> data;
    TimePoint sent_at;
    uint32_t send_count{1};
    uint32_t skip_count{0};  // fast retransmit: incremented when later packets are ACK'd
  };

  // Fragment header (4 bytes, present when kFlagFragment is set)
  struct FragmentHeader {
    uint16_t fragment_id;    // identifies the fragmented message
    uint8_t fragment_index;  // 0-based position
    uint8_t fragment_count;  // total fragments
  };

  struct FragmentGroup {
    uint8_t expected_count{0};
    uint8_t received_count{0};
    std::vector<std::byte> buffer;     // pre-allocated contiguous buffer
    std::vector<uint16_t> frag_sizes;  // per-fragment size (0 = not received)
    std::size_t total_size{0};
    TimePoint first_received;
  };

  // Build packet header and prepend to payload
  auto BuildPacket(uint8_t flags, SeqNum seq, std::span<const std::byte> payload,
                   const FragmentHeader* frag = nullptr) -> std::vector<std::byte>;

  // Rebuild packet header with fresh ACK info for retransmission
  auto RebuildPacketHeader(const std::vector<std::byte>& original_packet) -> std::vector<std::byte>;

  // ACK processing
  void ProcessAck(SeqNum ack_num, uint32_t ack_bits);

  // Resend logic
  void CheckResends();
  void StartResendTimer();
  void StopResendTimer();

  // Independent ACK sending + delayed ACK
  void SendAck();
  void ScheduleDelayedAck();
  void CancelDelayedAck();

  // Receive tracking
  void RecordReceivedSeq(SeqNum seq);
  auto IsDuplicate(SeqNum seq) const -> bool;

  // Fragmentation
  auto SendFragmented(std::span<const std::byte> payload) -> Result<void>;
  void OnFragmentReceived(const FragmentHeader& hdr, std::span<const std::byte> payload);
  void CleanupStaleFragments();

  // Ordered delivery
  void EnqueueForDelivery(SeqNum seq, std::span<const std::byte> payload, bool is_fragment,
                          const FragmentHeader& frag_hdr);
  void FlushReceiveBuffer();

  // Congestion control
  void OnAckCwndUpdate(uint32_t acked_count);
  void OnLossCwndUpdate(bool is_timeout);

  Socket& shared_socket_;

  // Sending state
  SeqNum next_send_seq_{1};
  std::map<SeqNum, UnackedPacket> unacked_;
  uint32_t send_window_{256};

  // Receiving state — ACK tracking (tracks what we received, for telling sender)
  SeqNum remote_seq_{0};       // highest received seq from peer
  uint32_t recv_ack_bits_{0};  // bitmask of received before remote_seq_

  // Receiving state — ordered delivery (KCP-style rcv_buf → rcv_queue)
  struct RecvEntry {
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

#endif  // ATLAS_LIB_NETWORK_RELIABLE_UDP_H_
