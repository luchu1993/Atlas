#include "network/reliable_udp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "serialization/binary_stream.h"

namespace atlas {

// ============================================================================
// Construction / Destruction
// ============================================================================

ReliableUdpChannel::ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                                       Socket& shared_socket, const Address& remote)
    : Channel(dispatcher, table, remote), shared_socket_(shared_socket) {}

ReliableUdpChannel::~ReliableUdpChannel() {
  CancelDelayedAck();
  StopResendTimer();
}

// ============================================================================
// effective_window — min(send_window, cwnd) or just send_window if nocwnd
// ============================================================================

auto ReliableUdpChannel::EffectiveWindow() const -> uint32_t {
  if (nocwnd_) {
    return send_window_;
  }
  return std::min(send_window_, cwnd_);
}

// ============================================================================
// send_reliable
// ============================================================================

auto ReliableUdpChannel::SendReliable() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

  if (bundle_.empty()) {
    return Result<void>{};
  }

  auto eff_wnd = EffectiveWindow();
  if (unacked_.size() >= eff_wnd) {
    return Error(ErrorCode::kWouldBlock, "Send window full");
  }

  auto payload = bundle_.Finalize();

  // Auto-fragment if payload exceeds max UDP payload
  if (payload.size() > rudp::kMaxUdpPayload) {
    return SendFragmented(payload);
  }

  auto seq = next_send_seq_++;

  uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
  if (remote_seq_ > 0) {
    flags |= rudp::kFlagHasAck;
  }

  CancelDelayedAck();
  auto packet = BuildPacket(flags, seq, std::span<const std::byte>(payload.data(), payload.size()));

  // Store for potential retransmission
  unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

  auto result = shared_socket_.SendTo(packet, remote_);
  if (!result) {
    return result.Error();
  }
  bytes_sent_ += *result;

  StartResendTimer();
  return Result<void>{};
}

// ============================================================================
// send_unreliable
// ============================================================================

auto ReliableUdpChannel::SendUnreliable() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

  if (bundle_.empty()) {
    return Result<void>{};
  }

  auto payload = bundle_.Finalize();

  uint8_t flags = 0;
  if (remote_seq_ > 0) {
    flags |= rudp::kFlagHasAck;  // piggyback ACK even on unreliable
  }

  CancelDelayedAck();
  auto packet = BuildPacket(flags, 0, std::span<const std::byte>(payload.data(), payload.size()));

  auto result = shared_socket_.SendTo(packet, remote_);
  if (!result) {
    return result.Error();
  }
  bytes_sent_ += *result;
  return Result<void>{};
}

// ============================================================================
// do_send (Channel virtual — treats raw data as reliable)
// ============================================================================

auto ReliableUdpChannel::DoSend(std::span<const std::byte> data) -> Result<size_t> {
  // Auto-fragment if payload exceeds max UDP payload
  if (data.size() > rudp::kMaxUdpPayload) {
    auto result = SendFragmented(data);
    if (!result) return result.Error();
    return data.size();
  }

  auto seq = next_send_seq_++;

  uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
  if (remote_seq_ > 0) {
    flags |= rudp::kFlagHasAck;
  }

  CancelDelayedAck();
  auto packet = BuildPacket(flags, seq, data);

  unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

  auto result = shared_socket_.SendTo(packet, remote_);
  if (!result) {
    return result.Error();
  }

  StartResendTimer();
  return *result;
}

void ReliableUdpChannel::OnCondemned() {
  CancelDelayedAck();
  StopResendTimer();
  unacked_.clear();
  rcv_buf_.clear();
  pending_fragments_.clear();
  ack_pending_ = false;
}

// ============================================================================
// build_packet
// ============================================================================

auto ReliableUdpChannel::BuildPacket(uint8_t flags, SeqNum seq, std::span<const std::byte> payload,
                                     const FragmentHeader* frag) -> std::vector<std::byte> {
  std::array<std::byte, rudp::kMaxHeaderSize> hdr_buf{};
  std::size_t hdr_len = 0;

  hdr_buf[hdr_len++] = static_cast<std::byte>(flags);

  if (flags & rudp::kFlagHasSeq) {
    auto le = endian::ToLittle(static_cast<uint32_t>(seq));
    std::memcpy(hdr_buf.data() + hdr_len, &le, sizeof(uint32_t));
    hdr_len += sizeof(uint32_t);
  }

  if (flags & rudp::kFlagHasAck) {
    auto ack_le = endian::ToLittle(static_cast<uint32_t>(remote_seq_));
    std::memcpy(hdr_buf.data() + hdr_len, &ack_le, sizeof(uint32_t));
    hdr_len += sizeof(uint32_t);
    auto bits_le = endian::ToLittle(recv_ack_bits_);
    std::memcpy(hdr_buf.data() + hdr_len, &bits_le, sizeof(uint32_t));
    hdr_len += sizeof(uint32_t);
  }

  if ((flags & rudp::kFlagFragment) && frag) {
    auto fid_le = endian::ToLittle(frag->fragment_id);
    std::memcpy(hdr_buf.data() + hdr_len, &fid_le, sizeof(uint16_t));
    hdr_len += sizeof(uint16_t);
    hdr_buf[hdr_len++] = static_cast<std::byte>(frag->fragment_index);
    hdr_buf[hdr_len++] = static_cast<std::byte>(frag->fragment_count);
  }

  std::vector<std::byte> pkt;
  pkt.reserve(hdr_len + payload.size());
  pkt.insert(pkt.end(), hdr_buf.data(), hdr_buf.data() + hdr_len);
  pkt.insert(pkt.end(), payload.begin(), payload.end());
  return pkt;
}

// ============================================================================
// on_datagram_received
// ============================================================================

void ReliableUdpChannel::OnDatagramReceived(std::span<const std::byte> data) {
  if (data.empty()) {
    return;
  }

  // Transport-level drop injection (script_client_smoke.md 场景 3). The drop
  // happens BEFORE header parsing, ACK generation, or receive-window
  // tracking — exactly where a transport-layer packet loss would. The
  // sender's unacked queue will see no ACK and retransmit, so reliable
  // traffic recovers automatically. Disabled when drop_duration_ms_ is 0.
  if (drop_duration_ms_ > 0) {
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - drop_origin_).count();
    if (elapsed_ms >= static_cast<int64_t>(drop_start_ms_) &&
        elapsed_ms < static_cast<int64_t>(drop_start_ms_ + drop_duration_ms_)) {
      return;
    }
  }

  BinaryReader reader(data);

  auto flags_result = reader.Read<uint8_t>();
  if (!flags_result) {
    return;
  }
  uint8_t flags = *flags_result;

  SeqNum seq = 0;
  if (flags & rudp::kFlagHasSeq) {
    auto s = reader.Read<uint32_t>();
    if (!s) {
      return;
    }
    seq = *s;
  }

  if (flags & rudp::kFlagHasAck) {
    auto ack_num = reader.Read<uint32_t>();
    auto ack_bits = reader.Read<uint32_t>();
    if (!ack_num || !ack_bits) {
      return;
    }
    ProcessAck(*ack_num, *ack_bits);
  }

  // Read fragment header if present
  FragmentHeader frag_hdr{};
  bool is_fragment = (flags & rudp::kFlagFragment) != 0;
  if (is_fragment) {
    auto fid = reader.Read<uint16_t>();
    auto fidx = reader.Read<uint8_t>();
    auto fcnt = reader.Read<uint8_t>();
    if (!fid || !fidx || !fcnt) {
      return;
    }
    frag_hdr = {*fid, *fidx, *fcnt};
  }

  // Remaining data is the payload
  auto remaining = reader.Remaining();
  if (remaining == 0) {
    // Zero-payload packet: still need to record seq for reliable packets
    // so the sender receives an ACK and does not retransmit forever.
    if (flags & rudp::kFlagHasSeq) {
      if (!IsDuplicate(seq) && !SeqGreaterThan(seq, rcv_nxt_ + rcv_wnd_)) {
        RecordReceivedSeq(seq);
        ScheduleDelayedAck();
      }
    }
    return;
  }

  auto payload = reader.ReadBytes(remaining);
  if (!payload) {
    return;
  }

  if (flags & rudp::kFlagHasSeq) {
    // Reliable packet -- check duplicate and receive window
    if (IsDuplicate(seq)) {
      ATLAS_LOG_DEBUG("Duplicate packet seq={} from {}", seq, remote_.ToString());
      return;
    }

    // Receive window check: drop if too far ahead of delivery frontier
    if (SeqGreaterThan(seq, rcv_nxt_ + rcv_wnd_)) {
      ATLAS_LOG_DEBUG("Packet seq={} outside receive window (nxt={}, wnd={})", seq, rcv_nxt_,
                      rcv_wnd_);
      return;
    }

    RecordReceivedSeq(seq);
    bytes_received_ += remaining;

    EnqueueForDelivery(seq, *payload, is_fragment, frag_hdr);
    FlushReceiveBuffer();

    if (seq != rcv_nxt_ - 1) {
      SendAck();
    } else {
      ScheduleDelayedAck();
    }
  } else {
    // Unreliable packet — dispatch immediately (no ordering guarantee)
    bytes_received_ += remaining;
    OnDataReceived(*payload);
    DispatchMessages(*payload);
  }
}

// ============================================================================
// process_ack
// ============================================================================

void ReliableUdpChannel::ProcessAck(SeqNum ack_num, uint32_t ack_bits) {
  std::array<SeqNum, 33> acked_seqs{};
  uint32_t acked_count = 0;
  auto now = Clock::now();

  for (int32_t diff = 0; diff <= 32; ++diff) {
    SeqNum candidate = ack_num - static_cast<SeqNum>(diff);
    if (diff > 0 && !(ack_bits & (1u << (diff - 1)))) {
      continue;
    }

    auto it = unacked_.find(candidate);
    if (it == unacked_.end()) {
      continue;
    }

    if (it->second.send_count == 1) {
      rtt_.Update(now - it->second.sent_at);
    }
    acked_seqs[acked_count++] = candidate;
    unacked_.erase(it);
  }

  if (acked_count != 0) {
    OnAckCwndUpdate(acked_count);
  }

  bool fast_resent = false;
  if (fast_resend_thresh_ > 0 && acked_count != 0) {
    SeqNum max_acked = acked_seqs[0];
    for (uint32_t i = 1; i < acked_count; ++i) {
      SeqNum s = acked_seqs[i];
      if (SeqLessThan(max_acked, s)) {
        max_acked = s;
      }
    }

    for (auto& [seq, pkt] : unacked_) {
      if (SeqLessThan(seq, max_acked)) {
        pkt.skip_count++;
      }

      if (pkt.skip_count >= fast_resend_thresh_) {
        auto fresh_pkt = RebuildPacketHeader(pkt.data);
        auto result = shared_socket_.SendTo(fresh_pkt, remote_);
        if (result) {
          bytes_sent_ += *result;
        }
        pkt.sent_at = now;
        pkt.send_count++;
        pkt.skip_count = 0;

        ATLAS_LOG_DEBUG("Fast resend seq={} to {}", seq, remote_.ToString());
        fast_resent = true;
      }
    }

    if (fast_resent) {
      OnLossCwndUpdate(false);
    }
  }

  if (unacked_.empty()) {
    StopResendTimer();
  }
}

// ============================================================================
// record_received_seq
// ============================================================================

void ReliableUdpChannel::RecordReceivedSeq(SeqNum seq) {
  if (SeqGreaterThan(seq, remote_seq_)) {
    // New highest -- shift ack_bits
    auto shift = static_cast<uint32_t>(seq - remote_seq_);
    if (shift < 32) {
      recv_ack_bits_ <<= shift;
      // Set bit for old remote_seq_ (which is now shift positions back)
      if (remote_seq_ > 0) {
        recv_ack_bits_ |= (1u << (shift - 1));
      }
    } else {
      recv_ack_bits_ = 0;
      if (remote_seq_ > 0 && shift == 32) {
        recv_ack_bits_ = (1u << 31);
      }
    }
    remote_seq_ = seq;
  } else {
    // Older packet -- set bit in ack_bits
    AckBitsSet(recv_ack_bits_, remote_seq_, seq);
  }
}

// ============================================================================
// is_duplicate
// ============================================================================

auto ReliableUdpChannel::IsDuplicate(SeqNum seq) const -> bool {
  if (seq == 0) {
    return false;
  }
  if (SeqLessThan(seq, remote_seq_)) {
    // Old packet -- check if in ack_bits window
    auto diff = static_cast<int32_t>(remote_seq_ - seq);
    if (diff > 32) {
      return true;  // too old
    }
    return AckBitsTest(recv_ack_bits_, remote_seq_, seq);
  }
  if (seq == remote_seq_) {
    return true;  // exact duplicate
  }
  return false;  // new packet
}

// ============================================================================
// enqueue_for_delivery — store in receive buffer for ordered delivery
// ============================================================================

void ReliableUdpChannel::EnqueueForDelivery(SeqNum seq, std::span<const std::byte> payload,
                                            bool is_fragment, const FragmentHeader& frag_hdr) {
  // Already delivered (seq < rcv_nxt_) — should have been caught by IsDuplicate
  if (SeqLessThan(seq, rcv_nxt_)) {
    return;
  }

  // Already in buffer
  if (rcv_buf_.count(seq) != 0) {
    return;
  }

  rcv_buf_[seq] =
      RecvEntry{std::vector<std::byte>(payload.begin(), payload.end()), is_fragment, frag_hdr};
}

// ============================================================================
// flush_receive_buffer — deliver consecutive packets starting from rcv_nxt_
// ============================================================================

void ReliableUdpChannel::FlushReceiveBuffer() {
  while (true) {
    auto it = rcv_buf_.find(rcv_nxt_);
    if (it == rcv_buf_.end()) {
      break;  // gap — waiting for rcv_nxt_ to arrive
    }

    auto entry = std::move(it->second);
    rcv_buf_.erase(it);
    rcv_nxt_++;

    if (entry.is_fragment) {
      // Buffer the fragment for reassembly
      OnFragmentReceived(entry.frag_hdr, std::span<const std::byte>(entry.payload));
    } else {
      // Non-fragment: dispatch immediately
      OnDataReceived(entry.payload);
      DispatchMessages(entry.payload);
    }
  }
}

// ============================================================================
// send_fragmented — split payload into MTU-sized reliable fragments
// ============================================================================

auto ReliableUdpChannel::SendFragmented(std::span<const std::byte> payload) -> Result<void> {
  auto frag_payload_size = rudp::kMaxUdpPayload;
  auto total = (payload.size() + frag_payload_size - 1) / frag_payload_size;

  if (total > rudp::kMaxFragments) {
    return Error(ErrorCode::kMessageTooLarge,
                 std::format("Message too large for fragmentation: {} bytes ({} fragments, max {})",
                             payload.size(), total, rudp::kMaxFragments));
  }

  if (unacked_.size() + total > EffectiveWindow()) {
    return Error(ErrorCode::kWouldBlock, "Send window too small for fragmented message");
  }

  // Skip IDs that collide with in-progress fragment reassembly
  auto frag_id = next_fragment_id_++;
  while (pending_fragments_.contains(frag_id)) frag_id = next_fragment_id_++;
  auto frag_count = static_cast<uint8_t>(total);

  // Record rollback point in case of partial send failure
  auto rollback_seq = next_send_seq_;

  for (uint8_t i = 0; i < frag_count; ++i) {
    auto offset = static_cast<std::size_t>(i) * frag_payload_size;
    auto chunk_size = std::min(frag_payload_size, payload.size() - offset);
    auto chunk = payload.subspan(offset, chunk_size);

    auto seq = next_send_seq_++;

    uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq | rudp::kFlagFragment;
    if (remote_seq_ > 0) {
      flags |= rudp::kFlagHasAck;
    }

    CancelDelayedAck();
    FragmentHeader fhdr{frag_id, i, frag_count};
    auto packet = BuildPacket(flags, seq, chunk, &fhdr);

    unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

    auto result = shared_socket_.SendTo(packet, remote_);
    if (!result) {
      // Roll back: remove all fragments we just enqueued in unacked_
      for (auto s = rollback_seq; s != next_send_seq_; ++s) unacked_.erase(s);
      next_send_seq_ = rollback_seq;
      return result.Error();
    }
    bytes_sent_ += *result;
  }

  StartResendTimer();
  return Result<void>{};
}

// ============================================================================
// on_fragment_received — buffer and reassemble fragments
// ============================================================================

void ReliableUdpChannel::OnFragmentReceived(const FragmentHeader& hdr,
                                            std::span<const std::byte> payload) {
  if (hdr.fragment_count == 0 || hdr.fragment_index >= hdr.fragment_count) {
    ATLAS_LOG_WARNING("Invalid fragment header from {}", remote_.ToString());
    return;
  }

  auto& group = pending_fragments_[hdr.fragment_id];

  if (group.expected_count == 0) {
    group.expected_count = hdr.fragment_count;
    group.buffer.reserve(hdr.fragment_count * rudp::kMaxUdpPayload);
    group.frag_sizes.resize(hdr.fragment_count, 0);
    group.first_received = Clock::now();

    if (pending_fragments_.size() > kMaxPendingFragmentGroups) {
      CleanupStaleFragments();
    }
  } else if (group.expected_count != hdr.fragment_count) {
    ATLAS_LOG_WARNING("Fragment count mismatch for id={}", hdr.fragment_id);
    return;
  }

  if (group.frag_sizes[hdr.fragment_index] == 0) {
    auto offset = static_cast<std::size_t>(hdr.fragment_index) * rudp::kMaxUdpPayload;
    auto needed = offset + payload.size();
    if (group.buffer.size() < needed) {
      group.buffer.resize(needed);
    }
    std::memcpy(group.buffer.data() + offset, payload.data(), payload.size());
    group.frag_sizes[hdr.fragment_index] = static_cast<uint16_t>(payload.size());
    group.total_size += payload.size();
    group.received_count++;
  }

  if (group.received_count == group.expected_count) {
    // Compact into contiguous payload (fragments may have varying sizes)
    std::vector<std::byte> full_payload;
    full_payload.reserve(group.total_size);
    for (uint8_t i = 0; i < group.expected_count; ++i) {
      auto frag_offset = static_cast<std::size_t>(i) * rudp::kMaxUdpPayload;
      full_payload.insert(full_payload.end(), group.buffer.data() + frag_offset,
                          group.buffer.data() + frag_offset + group.frag_sizes[i]);
    }

    pending_fragments_.erase(hdr.fragment_id);

    OnDataReceived(full_payload);
    DispatchMessages(full_payload);
  }
}

// ============================================================================
// cleanup_stale_fragments — remove incomplete groups older than timeout
// ============================================================================

void ReliableUdpChannel::CleanupStaleFragments() {
  auto now = Clock::now();
  std::erase_if(pending_fragments_, [now](const auto& pair) {
    return (now - pair.second.first_received) >= rudp::kFragmentTimeout;
  });
}

// ============================================================================
// Independent ACK sending + delayed ACK
// ============================================================================

void ReliableUdpChannel::SendAck() {
  if (remote_seq_ == 0) {
    return;
  }

  uint8_t flags = rudp::kFlagHasAck;
  auto packet = BuildPacket(flags, 0, std::span<const std::byte>{});

  auto result = shared_socket_.SendTo(packet, remote_);
  if (result) {
    bytes_sent_ += *result;
  } else {
    ATLAS_LOG_DEBUG("RUDP send_ack failed to {}: {}", remote_.ToString(), result.Error().Message());
  }
  ack_pending_ = false;
}

void ReliableUdpChannel::ScheduleDelayedAck() {
  ack_pending_ = true;
  if (delayed_ack_timer_.IsValid()) {
    return;
  }
  delayed_ack_timer_ = dispatcher_.AddTimer(kDelayedAckTimeout, [this](TimerHandle) {
    delayed_ack_timer_ = TimerHandle{};
    if (ack_pending_) {
      SendAck();
    }
  });
}

void ReliableUdpChannel::CancelDelayedAck() {
  if (delayed_ack_timer_.IsValid()) {
    dispatcher_.CancelTimer(delayed_ack_timer_);
    delayed_ack_timer_ = TimerHandle{};
  }
  ack_pending_ = false;
}

// ============================================================================
// rebuild_packet_header — retransmit with fresh ACK info
// ============================================================================

auto ReliableUdpChannel::RebuildPacketHeader(const std::vector<std::byte>& original_packet)
    -> std::vector<std::byte> {
  if (original_packet.empty()) {
    return original_packet;
  }

  BinaryReader reader(std::span<const std::byte>(original_packet.data(), original_packet.size()));
  auto flags_result = reader.Read<uint8_t>();
  if (!flags_result) {
    return original_packet;
  }
  uint8_t flags = *flags_result;

  SeqNum seq = 0;
  if (flags & rudp::kFlagHasSeq) {
    auto s = reader.Read<uint32_t>();
    if (!s) {
      return original_packet;
    }
    seq = *s;
  }

  // Skip old ACK fields
  if (flags & rudp::kFlagHasAck) {
    reader.Skip(sizeof(uint32_t) * 2);
  }

  // Read fragment header if present
  FragmentHeader frag_hdr{};
  const FragmentHeader* frag_ptr = nullptr;
  if (flags & rudp::kFlagFragment) {
    auto fid = reader.Read<uint16_t>();
    auto fidx = reader.Read<uint8_t>();
    auto fcnt = reader.Read<uint8_t>();
    if (fid && fidx && fcnt) {
      frag_hdr = {*fid, *fidx, *fcnt};
      frag_ptr = &frag_hdr;
    }
  }

  auto payload_bytes = reader.ReadBytes(reader.Remaining());
  if (!payload_bytes) {
    return original_packet;
  }

  // Always include ACK in retransmissions
  uint8_t new_flags = flags;
  if (remote_seq_ > 0) {
    new_flags |= rudp::kFlagHasAck;
  }

  return BuildPacket(new_flags, seq, *payload_bytes, frag_ptr);
}

// ============================================================================
// check_resends
// ============================================================================

void ReliableUdpChannel::CheckResends() {
  auto now = Clock::now();

  // Periodically clean up stale fragment groups
  if (!pending_fragments_.empty()) {
    CleanupStaleFragments();
  }

  bool had_timeout = false;
  for (auto& [seq, pkt] : unacked_) {
    auto backoff = rtt_.Rto();
    for (uint32_t i = 1; i < pkt.send_count && i < 5; ++i) {
      if (rtt_.Nodelay()) {
        backoff = backoff * 3 / 2;
      } else {
        backoff *= 2;
      }
    }

    if (now - pkt.sent_at >= backoff) {
      auto fresh_pkt = RebuildPacketHeader(pkt.data);
      auto result = shared_socket_.SendTo(fresh_pkt, remote_);
      if (result) {
        bytes_sent_ += *result;
      }
      pkt.sent_at = now;
      pkt.send_count++;
      had_timeout = true;

      ATLAS_LOG_DEBUG("Resend seq={} attempt={} to {}", seq, pkt.send_count, remote_.ToString());
    }
  }

  if (had_timeout) {
    OnLossCwndUpdate(true);
  }
}

// ============================================================================
// Congestion control (KCP-style: slow start + congestion avoidance)
// ============================================================================

void ReliableUdpChannel::OnAckCwndUpdate(uint32_t acked_count) {
  if (nocwnd_) {
    return;
  }

  for (uint32_t i = 0; i < acked_count; ++i) {
    if (cwnd_ < ssthresh_) {
      // Slow start: cwnd grows by 1 per ACK (doubles per RTT)
      cwnd_++;
      cwnd_incr_ += rudp::kMtu;
    } else {
      // Congestion avoidance (KCP formula):
      // incr += mss * mss / incr + mss / 16
      if (cwnd_incr_ == 0) {
        cwnd_incr_ = rudp::kMtu;
      }
      cwnd_incr_ += static_cast<uint32_t>(rudp::kMtu * rudp::kMtu / cwnd_incr_ + rudp::kMtu / 16);

      // cwnd = incr / mss
      auto new_cwnd = cwnd_incr_ / static_cast<uint32_t>(rudp::kMtu);
      if (new_cwnd > cwnd_) {
        cwnd_ = new_cwnd;
      }
    }
  }

  // Cap at send_window_
  if (cwnd_ > send_window_) {
    cwnd_ = send_window_;
    cwnd_incr_ = send_window_ * static_cast<uint32_t>(rudp::kMtu);
  }
}

void ReliableUdpChannel::OnLossCwndUpdate(bool is_timeout) {
  if (nocwnd_) {
    return;
  }

  auto in_flight = static_cast<uint32_t>(unacked_.size());

  if (is_timeout) {
    // RTO timeout: aggressive reduction (TCP Tahoe style)
    ssthresh_ = std::max(cwnd_ / 2, 2u);
    cwnd_ = 1;
    cwnd_incr_ = rudp::kMtu;
  } else {
    // Fast retransmit: moderate reduction (TCP Reno style)
    // ssthresh = max(in_flight / 2, 2)
    ssthresh_ = std::max(in_flight / 2, 2u);
    cwnd_ = ssthresh_ + fast_resend_thresh_;
    cwnd_incr_ = cwnd_ * static_cast<uint32_t>(rudp::kMtu);
  }
}

// ============================================================================
// Resend timer management
// ============================================================================

void ReliableUdpChannel::StartResendTimer() {
  // Only start if not already running — avoids wasteful cancel+recreate cycles
  if (resend_timer_.IsValid()) return;

  auto min_interval = rtt_.Nodelay() ? Milliseconds(10) : Milliseconds(50);
  auto interval = std::max(rtt_.Rto(), Duration(min_interval));
  resend_timer_ = dispatcher_.AddRepeatingTimer(interval, [this](TimerHandle) { CheckResends(); });
}

void ReliableUdpChannel::StopResendTimer() {
  if (resend_timer_.IsValid()) {
    dispatcher_.CancelTimer(resend_timer_);
    resend_timer_ = TimerHandle{};
  }
}

}  // namespace atlas
