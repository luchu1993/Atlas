#ifndef ATLAS_LIB_NETWORK_RELIABLE_UDP_H_
#define ATLAS_LIB_NETWORK_RELIABLE_UDP_H_

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "foundation/timer_queue.h"
#include "network/bundle.h"
#include "network/channel.h"
#include "network/rtt_estimator.h"
#include "network/seq_num.h"
#include "network/socket.h"

namespace atlas {

namespace rudp {
inline constexpr uint8_t kFlagReliable = 0x01;
inline constexpr uint8_t kFlagHasSeq = 0x02;
inline constexpr uint8_t kFlagHasAck = 0x04;
inline constexpr uint8_t kFlagFragment = 0x08;

// flags + seq + ack + ack_bits + una + fragment header.
inline constexpr std::size_t kMaxHeaderSize = 21;

// Both endpoints must agree; reassembly assumes uniform chunk size.
inline constexpr std::size_t kDefaultMtu = 1400;
inline constexpr std::size_t kMaxFragments = 255;
inline constexpr Duration kFragmentTimeout = std::chrono::seconds(30);
}  // namespace rudp

// Not thread-safe; used from EventDispatcher's thread only.
class ReliableUdpChannel : public Channel {
 public:
  ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
                     const Address& remote);
  ~ReliableUdpChannel() override;

  [[nodiscard]] auto Fd() const -> FdHandle override { return shared_socket_.Fd(); }

  [[nodiscard]] auto SendReliable() -> Result<void>;
  [[nodiscard]] auto SendUnreliable() -> Result<void> override;

  // OOM safety net; FlushDirtySendChannels is the primary drain.
  void SetDeferredFlushThreshold(std::size_t bytes) { deferred_flush_threshold_ = bytes; }
  [[nodiscard]] auto DeferredFlushThreshold() const -> std::size_t {
    return deferred_flush_threshold_;
  }

  [[nodiscard]] auto FlushDeferred() -> Result<void> override;

  [[nodiscard]] auto HasDeferredPayload() const -> bool {
    return !deferred_reliable_bundle_.empty() || !deferred_unreliable_bundle_.empty();
  }

  // flush_deadline bounds in-order delivery cascades.
  void OnDatagramReceived(std::span<const std::byte> data,
                          TimePoint flush_deadline = TimePoint::max());

  using HotCallback = std::function<void(ReliableUdpChannel&)>;
  void SetHotCallback(HotCallback cb) { hot_cb_ = std::move(cb); }

  [[nodiscard]] auto FlushReceiveBuffer(TimePoint deadline) -> bool;

  [[nodiscard]] auto HasReceiveBacklog() const -> bool {
    return HasPendingDispatch() || rcv_buf_.find(rcv_nxt_) != rcv_buf_.end();
  }

  // Test hook: drops inbound datagrams before header parsing.
  void SetInboundDropWindow(TimePoint origin, uint32_t start_ms, uint32_t duration_ms) {
    drop_origin_ = origin;
    drop_start_ms_ = start_ms;
    drop_duration_ms_ = duration_ms;
  }

  void SetNodelay(bool enable) { rtt_.SetNodelay(enable); }
  [[nodiscard]] auto Nodelay() const -> bool { return rtt_.Nodelay(); }

  void SetFastResendThresh(uint32_t thresh) { fast_resend_thresh_ = thresh; }

  void SetSendWindow(uint32_t wnd) { send_window_ = wnd; }
  void SetRecvWindow(uint32_t wnd) { rcv_wnd_ = wnd; }
  [[nodiscard]] auto RecvWindow() const -> uint32_t { return rcv_wnd_; }

  // Both endpoints must agree.
  void SetMtu(std::size_t mtu) { mtu_ = mtu; }
  [[nodiscard]] auto Mtu() const -> std::size_t { return mtu_; }
  [[nodiscard]] auto MaxUdpPayload() const -> std::size_t { return mtu_ - rudp::kMaxHeaderSize; }

  void SetNocwnd(bool enable) { nocwnd_ = enable; }
  [[nodiscard]] auto Nocwnd() const -> bool { return nocwnd_; }

  [[nodiscard]] auto Rtt() const -> Duration { return rtt_.Rtt(); }
  [[nodiscard]] auto UnackedCount() const -> uint32_t {
    return static_cast<uint32_t>(unacked_.size());
  }
  [[nodiscard]] auto HasUnackedReliablePackets() const -> bool override {
    return !unacked_.empty();
  }
  [[nodiscard]] auto HasRemoteFailed() const -> bool override { return remote_failed_; }
  [[nodiscard]] auto RecvNextSeq() const -> SeqNum { return rcv_nxt_; }
  [[nodiscard]] auto RecvBufCount() const -> uint32_t {
    return static_cast<uint32_t>(rcv_buf_.size());
  }
  [[nodiscard]] auto Cwnd() const -> uint32_t { return cwnd_; }
  [[nodiscard]] auto Ssthresh() const -> uint32_t { return ssthresh_; }
  [[nodiscard]] auto EffectiveWindow() const -> uint32_t;
  [[nodiscard]] auto PendingFragmentGroupCount() const -> uint32_t {
    return static_cast<uint32_t>(pending_fragments_.size());
  }

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;
  void OnCondemned() override;

  [[nodiscard]] auto DeferredBundleFor(const MessageDesc& desc) -> class Bundle* override;
  [[nodiscard]] auto OnDeferredAppend(const MessageDesc& desc, std::size_t total_size)
      -> Result<void> override;

 private:
  struct UnackedPacket {
    std::vector<std::byte> data;
    TimePoint sent_at;
    TimePoint first_sent_at;
    uint32_t send_count{1};
    uint32_t skip_count{0};
  };

  struct FragmentHeader {
    uint16_t fragment_id;
    uint8_t fragment_index;
    uint8_t fragment_count;
  };

  struct FragmentGroup {
    uint8_t expected_count{0};
    uint8_t received_count{0};
    std::vector<std::byte> buffer;
    std::vector<uint16_t> frag_sizes;
    std::size_t total_size{0};
    TimePoint first_received;
  };

  auto BuildPacket(uint8_t flags, SeqNum seq, std::span<const std::byte> payload,
                   const FragmentHeader* frag = nullptr) -> std::vector<std::byte>;

  auto RebuildPacketHeader(const std::vector<std::byte>& original_packet) -> std::vector<std::byte>;

  auto ProcessUna(SeqNum una) -> uint32_t;
  void ProcessAck(SeqNum ack_num, uint32_t ack_bits);

  void CheckResends();
  void StartResendTimer();
  void StopResendTimer();

  [[nodiscard]] auto SendBundleReliable(class Bundle& b) -> Result<void>;
  [[nodiscard]] auto SendBundleUnreliable(class Bundle& b) -> Result<void>;

  void SendAck();
  void ScheduleDelayedAck();
  void CancelDelayedAck();

  void RecordReceivedSeq(SeqNum seq);
  auto IsDuplicate(SeqNum seq) const -> bool;

  auto SendFragmented(std::span<const std::byte> payload) -> Result<void>;
  void OnFragmentReceived(const FragmentHeader& hdr, std::span<const std::byte> payload);
  void CleanupStaleFragments();

  void EnqueueForDelivery(SeqNum seq, std::span<const std::byte> payload, bool is_fragment,
                          const FragmentHeader& frag_hdr);

  auto DispatchFiltered(std::span<const std::byte> payload, TimePoint deadline = TimePoint::max())
      -> bool;

  void OnAckCwndUpdate(uint32_t acked_count);
  void OnLossCwndUpdate(bool is_timeout);

  Socket& shared_socket_;

  std::size_t mtu_{rudp::kDefaultMtu};
  std::size_t deferred_flush_threshold_{60 * 1024};

  SeqNum next_send_seq_{1};
  std::map<SeqNum, UnackedPacket> unacked_;
  uint32_t send_window_{256};

  class Bundle deferred_reliable_bundle_;
  class Bundle deferred_unreliable_bundle_;

  SeqNum remote_seq_{0};
  uint32_t recv_ack_bits_{0};

  struct RecvEntry {
    std::vector<std::byte> payload;
    bool is_fragment{false};
    FragmentHeader frag_hdr{};
  };
  std::map<SeqNum, RecvEntry> rcv_buf_;
  SeqNum rcv_nxt_{1};
  uint32_t rcv_wnd_{256};

  HotCallback hot_cb_;

  RttEstimator rtt_;

  uint32_t fast_resend_thresh_{2};

  static constexpr Duration kDeadLinkTimeout = std::chrono::seconds(5);
  bool remote_failed_{false};

  bool nocwnd_{false};
  uint32_t cwnd_{1};
  uint32_t ssthresh_{16};
  uint32_t cwnd_incr_{0};

  uint16_t next_fragment_id_{1};
  std::unordered_map<uint16_t, FragmentGroup> pending_fragments_;
  // Bounds fragment_id fan-out.
  static constexpr std::size_t kMaxPendingFragmentGroups = 16;

  TimerHandle resend_timer_;

  TimerHandle delayed_ack_timer_;
  bool ack_pending_{false};
  static constexpr Duration kDelayedAckTimeout = std::chrono::milliseconds(10);

  TimePoint drop_origin_{};
  uint32_t drop_start_ms_{0};
  uint32_t drop_duration_ms_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_RELIABLE_UDP_H_
