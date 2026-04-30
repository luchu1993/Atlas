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

ReliableUdpChannel::ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                                       Socket& shared_socket, const Address& remote)
    : Channel(dispatcher, table, remote), shared_socket_(shared_socket) {}

ReliableUdpChannel::~ReliableUdpChannel() {
  CancelDelayedAck();
  StopResendTimer();
}

auto ReliableUdpChannel::EffectiveWindow() const -> uint32_t {
  if (nocwnd_) {
    return send_window_;
  }
  return std::min(send_window_, cwnd_);
}

auto ReliableUdpChannel::SendReliable() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    bundle_.Clear();
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }
  return SendBundleReliable(bundle_);
}

auto ReliableUdpChannel::SendUnreliable() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    bundle_.Clear();
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }
  return SendBundleUnreliable(bundle_);
}

auto ReliableUdpChannel::DeferredBundleFor(const MessageDesc& desc) -> class Bundle* {
  if (state_ == ChannelState::kCondemned) return nullptr;
  return desc.IsUnreliable() ? &deferred_unreliable_bundle_ : &deferred_reliable_bundle_;
}

auto ReliableUdpChannel::OnDeferredAppend(const MessageDesc& desc, std::size_t total_size)
    -> Result<void> {
  NotifyDirty();
  if (total_size >= deferred_flush_threshold_) {
    auto& bundle = desc.IsUnreliable() ? deferred_unreliable_bundle_ : deferred_reliable_bundle_;
    auto r = desc.IsUnreliable() ? SendBundleUnreliable(bundle) : SendBundleReliable(bundle);
    if (!r) return r.Error();
  }
  return Result<void>{};
}

auto ReliableUdpChannel::FlushDeferred() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    deferred_reliable_bundle_.Clear();
    deferred_unreliable_bundle_.Clear();
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

  Result<void> first_err{};
  if (!deferred_unreliable_bundle_.empty()) {
    auto r = SendBundleUnreliable(deferred_unreliable_bundle_);
    if (!r) first_err = r.Error();
  }
  if (!deferred_reliable_bundle_.empty()) {
    auto r = SendBundleReliable(deferred_reliable_bundle_);
    if (!r && first_err.HasValue()) first_err = r.Error();
  }
  return first_err;
}

auto ReliableUdpChannel::SendBundleReliable(class Bundle& b) -> Result<void> {
  if (b.empty()) {
    return Result<void>{};
  }

  const auto eff_wnd = EffectiveWindow();
  const auto frag_size = MaxUdpPayload();
  const auto bundle_size = b.TotalSize();
  if (bundle_size > frag_size) {
    const auto packets_needed = (bundle_size + frag_size - 1) / frag_size;
    if (packets_needed > rudp::kMaxFragments) {
      return Error(
          ErrorCode::kMessageTooLarge,
          std::format("Bundle too large for fragmentation: {} bytes ({} fragments, max {})",
                      bundle_size, packets_needed, rudp::kMaxFragments));
    }
    if (unacked_.size() + packets_needed > eff_wnd) {
      return Error(ErrorCode::kWouldBlock, "Send window too small for fragmented bundle");
    }
  } else {
    if (unacked_.size() >= eff_wnd) {
      return Error(ErrorCode::kWouldBlock, "Send window full");
    }
  }

  auto payload = b.Finalize();

  if (packet_filter_) {
    auto filtered =
        packet_filter_->SendFilter(std::span<const std::byte>(payload.data(), payload.size()));
    if (!filtered) {
      ATLAS_LOG_WARNING("RUDP send_filter failed for {}: {} ({} bytes lost)", remote_.ToString(),
                        filtered.Error().Message(), payload.size());
      return filtered.Error();
    }
    payload = std::move(*filtered);
  }

  if (payload.size() > MaxUdpPayload()) {
    return SendFragmented(payload);
  }

  auto seq = next_send_seq_++;

  uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
  if (remote_seq_ > 0) {
    flags |= rudp::kFlagHasAck;
  }

  CancelDelayedAck();
  auto packet = BuildPacket(flags, seq, std::span<const std::byte>(payload.data(), payload.size()));

  const auto now = Clock::now();
  auto& slot = unacked_[seq];
  slot = UnackedPacket{std::move(packet), now, now, 1};

  auto result = shared_socket_.SendTo(slot.data, remote_);
  if (!result) {
    unacked_.erase(seq);
    next_send_seq_ = seq;
    return result.Error();
  }
  bytes_sent_ += *result;

  StartResendTimer();
  return Result<void>{};
}

auto ReliableUdpChannel::SendBundleUnreliable(class Bundle& b) -> Result<void> {
  if (b.empty()) {
    return Result<void>{};
  }

  auto payload = b.Finalize();

  if (packet_filter_) {
    auto filtered =
        packet_filter_->SendFilter(std::span<const std::byte>(payload.data(), payload.size()));
    if (!filtered) {
      ATLAS_LOG_WARNING("RUDP send_filter failed (unreliable) for {}: {} ({} bytes lost)",
                        remote_.ToString(), filtered.Error().Message(), payload.size());
      return filtered.Error();
    }
    payload = std::move(*filtered);
  }

  uint8_t flags = 0;
  if (remote_seq_ > 0) {
    flags |= rudp::kFlagHasAck;
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

auto ReliableUdpChannel::DoSend(std::span<const std::byte> data) -> Result<size_t> {
  if (state_ == ChannelState::kCondemned) {
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

  if (data.size() > MaxUdpPayload()) {
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

  const auto now = Clock::now();
  auto& slot = unacked_[seq];
  slot = UnackedPacket{std::move(packet), now, now, 1};

  auto result = shared_socket_.SendTo(slot.data, remote_);
  if (!result) {
    unacked_.erase(seq);
    next_send_seq_ = seq;
    return result.Error();
  }

  StartResendTimer();
  return *result;
}

void ReliableUdpChannel::OnCondemned() {
  CancelDelayedAck();
  rcv_buf_.clear();
  pending_fragments_.clear();
  ack_pending_ = false;
  deferred_reliable_bundle_.Clear();
  deferred_unreliable_bundle_.Clear();
  pending_dispatch_buf_.clear();
  pending_dispatch_buf_.shrink_to_fit();
  pending_dispatch_pos_ = 0;
}

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
    auto una_le = endian::ToLittle(static_cast<uint32_t>(rcv_nxt_));
    std::memcpy(hdr_buf.data() + hdr_len, &una_le, sizeof(uint32_t));
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

void ReliableUdpChannel::OnDatagramReceived(std::span<const std::byte> data,
                                            TimePoint flush_deadline) {
  if (data.empty()) {
    return;
  }

  ResetInactivityTimer();

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
    auto una = reader.Read<uint32_t>();
    if (!ack_num || !ack_bits || !una) {
      return;
    }
    auto una_acked = ProcessUna(*una);
    if (una_acked != 0) {
      OnAckCwndUpdate(una_acked);
    }
    ProcessAck(*ack_num, *ack_bits);
    // Acks may free send-window slots for deferred kBatched payloads.
    if (HasDeferredPayload()) NotifyDirty();
  }

  if (state_ == ChannelState::kCondemned) {
    return;
  }

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

  auto remaining = reader.Remaining();
  if (remaining == 0) {
    if (flags & rudp::kFlagHasSeq) {
      if (!IsDuplicate(seq) && !SeqGreaterThan(seq, rcv_nxt_ + rcv_wnd_)) {
        RecordReceivedSeq(seq);
      }
      ScheduleDelayedAck();
    }
    return;
  }

  auto payload = reader.ReadBytes(remaining);
  if (!payload) {
    return;
  }

  if (flags & rudp::kFlagHasSeq) {
    if (IsDuplicate(seq)) {
      ATLAS_LOG_DEBUG("Duplicate packet seq={} from {}", seq, remote_.ToString());
      ScheduleDelayedAck();
      return;
    }
    if (SeqGreaterThan(seq, rcv_nxt_ + rcv_wnd_)) {
      ATLAS_LOG_DEBUG("Packet seq={} outside receive window (nxt={}, wnd={})", seq, rcv_nxt_,
                      rcv_wnd_);
      ScheduleDelayedAck();
      return;
    }

    RecordReceivedSeq(seq);
    bytes_received_ += remaining;

    EnqueueForDelivery(seq, *payload, is_fragment, frag_hdr);
    const bool more_pending = FlushReceiveBuffer(flush_deadline);
    if (more_pending && hot_cb_) {
      hot_cb_(*this);
    }

    if (seq != rcv_nxt_ - 1) {
      SendAck();
    } else {
      ScheduleDelayedAck();
    }
  } else {
    bytes_received_ += remaining;
    (void)DispatchFiltered(*payload);
  }
}

auto ReliableUdpChannel::DispatchFiltered(std::span<const std::byte> payload, TimePoint deadline)
    -> bool {
  std::vector<std::byte> filtered_storage;
  if (packet_filter_) {
    auto filtered = packet_filter_->RecvFilter(payload);
    if (!filtered) {
      ATLAS_LOG_WARNING("RUDP recv_filter failed from {}: {}", remote_.ToString(),
                        filtered.Error().Message());
      return false;
    }
    filtered_storage = std::move(*filtered);
    payload = std::span<const std::byte>(filtered_storage.data(), filtered_storage.size());
  }
  OnDataReceived(payload);
  return DispatchMessagesBudgeted(payload, deadline);
}

auto ReliableUdpChannel::ProcessUna(SeqNum una) -> uint32_t {
  uint32_t cleared = 0;
  bool have_rtt_sample = false;
  Duration rtt_sample{};
  const auto now = Clock::now();
  for (auto it = unacked_.begin(); it != unacked_.end();) {
    if (SeqLessThan(it->first, una)) {
      if (it->second.send_count == 1) {
        rtt_sample = now - it->second.sent_at;
        have_rtt_sample = true;
      }
      it = unacked_.erase(it);
      ++cleared;
    } else {
      ++it;
    }
  }
  if (have_rtt_sample) {
    rtt_.Update(rtt_sample);
  }
  if (cleared != 0 && unacked_.empty()) {
    StopResendTimer();
  }
  return cleared;
}

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

    if (diff == 0 && it->second.send_count == 1) {
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

void ReliableUdpChannel::RecordReceivedSeq(SeqNum seq) {
  if (SeqGreaterThan(seq, remote_seq_)) {
    auto shift = static_cast<uint32_t>(seq - remote_seq_);
    if (shift < 32) {
      recv_ack_bits_ <<= shift;
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
    AckBitsSet(recv_ack_bits_, remote_seq_, seq);
  }
}

// Uses delivery state, not SACK state; gaps can slide SACK past needed fills.
auto ReliableUdpChannel::IsDuplicate(SeqNum seq) const -> bool {
  if (SeqLessThan(seq, rcv_nxt_)) {
    return true;
  }
  if (rcv_buf_.count(seq) != 0) {
    return true;
  }
  return false;
}

void ReliableUdpChannel::EnqueueForDelivery(SeqNum seq, std::span<const std::byte> payload,
                                            bool is_fragment, const FragmentHeader& frag_hdr) {
  if (SeqLessThan(seq, rcv_nxt_)) {
    return;
  }

  if (rcv_buf_.count(seq) != 0) {
    return;
  }

  rcv_buf_[seq] =
      RecvEntry{std::vector<std::byte>(payload.begin(), payload.end()), is_fragment, frag_hdr};
}

auto ReliableUdpChannel::FlushReceiveBuffer(TimePoint deadline) -> bool {
  if (HasPendingDispatch()) {
    if (DrainPendingDispatch(deadline)) return true;
    if (Clock::now() >= deadline) {
      return rcv_buf_.find(rcv_nxt_) != rcv_buf_.end();
    }
  }

  while (true) {
    auto it = rcv_buf_.find(rcv_nxt_);
    if (it == rcv_buf_.end()) {
      return false;
    }

    auto entry = std::move(it->second);
    rcv_buf_.erase(it);
    rcv_nxt_++;

    if (entry.is_fragment) {
      OnFragmentReceived(entry.frag_hdr, std::span<const std::byte>(entry.payload));
    } else {
      if (DispatchFiltered(entry.payload, deadline)) return true;
    }

    if (Clock::now() >= deadline) {
      return rcv_buf_.find(rcv_nxt_) != rcv_buf_.end();
    }
  }
}

auto ReliableUdpChannel::SendFragmented(std::span<const std::byte> payload) -> Result<void> {
  auto frag_payload_size = MaxUdpPayload();
  auto total = (payload.size() + frag_payload_size - 1) / frag_payload_size;

  if (total > rudp::kMaxFragments) {
    return Error(ErrorCode::kMessageTooLarge,
                 std::format("Message too large for fragmentation: {} bytes ({} fragments, max {})",
                             payload.size(), total, rudp::kMaxFragments));
  }

  if (unacked_.size() + total > EffectiveWindow()) {
    return Error(ErrorCode::kWouldBlock, "Send window too small for fragmented message");
  }

  auto frag_id = next_fragment_id_++;
  auto frag_count = static_cast<uint8_t>(total);

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

    const auto now = Clock::now();
    auto& slot = unacked_[seq];
    slot = UnackedPacket{std::move(packet), now, now, 1};

    auto result = shared_socket_.SendTo(slot.data, remote_);
    if (!result) {
      for (auto s = rollback_seq; s != next_send_seq_; ++s) unacked_.erase(s);
      next_send_seq_ = rollback_seq;
      return result.Error();
    }
    bytes_sent_ += *result;
  }

  StartResendTimer();
  return Result<void>{};
}

void ReliableUdpChannel::OnFragmentReceived(const FragmentHeader& hdr,
                                            std::span<const std::byte> payload) {
  if (hdr.fragment_count == 0 || hdr.fragment_index >= hdr.fragment_count) {
    ATLAS_LOG_WARNING("Invalid fragment header from {}", remote_.ToString());
    return;
  }

  auto it = pending_fragments_.find(hdr.fragment_id);
  if (it == pending_fragments_.end()) {
    if (pending_fragments_.size() >= kMaxPendingFragmentGroups) {
      CleanupStaleFragments();
    }
    if (pending_fragments_.size() >= kMaxPendingFragmentGroups) {
      auto oldest = std::min_element(pending_fragments_.begin(), pending_fragments_.end(),
                                     [](const auto& a, const auto& b) {
                                       return a.second.first_received < b.second.first_received;
                                     });
      ATLAS_LOG_WARNING("Fragment table full from {}: evicting id={} (age first_received)",
                        remote_.ToString(), oldest->first);
      pending_fragments_.erase(oldest);
    }
    auto [inserted_it, _] = pending_fragments_.emplace(hdr.fragment_id, FragmentGroup{});
    it = inserted_it;
    auto& group_init = it->second;
    group_init.expected_count = hdr.fragment_count;
    group_init.frag_sizes.resize(hdr.fragment_count, 0);
    group_init.first_received = Clock::now();
  } else if (it->second.expected_count != hdr.fragment_count) {
    ATLAS_LOG_WARNING("Fragment count mismatch for id={}", hdr.fragment_id);
    return;
  }
  auto& group = it->second;

  if (group.frag_sizes[hdr.fragment_index] == 0) {
    auto offset = static_cast<std::size_t>(hdr.fragment_index) * MaxUdpPayload();
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
    std::vector<std::byte> full_payload;
    full_payload.reserve(group.total_size);
    for (uint8_t i = 0; i < group.expected_count; ++i) {
      auto frag_offset = static_cast<std::size_t>(i) * MaxUdpPayload();
      full_payload.insert(full_payload.end(), group.buffer.data() + frag_offset,
                          group.buffer.data() + frag_offset + group.frag_sizes[i]);
    }

    pending_fragments_.erase(hdr.fragment_id);

    (void)DispatchFiltered(full_payload);
  }
}

void ReliableUdpChannel::CleanupStaleFragments() {
  auto now = Clock::now();
  std::erase_if(pending_fragments_, [now](const auto& pair) {
    return (now - pair.second.first_received) >= rudp::kFragmentTimeout;
  });
}

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

  if (flags & rudp::kFlagHasAck) {
    reader.Skip(sizeof(uint32_t) * 3);
  }

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

  uint8_t new_flags = flags;
  if (remote_seq_ > 0) {
    new_flags |= rudp::kFlagHasAck;
  }

  return BuildPacket(new_flags, seq, *payload_bytes, frag_ptr);
}

void ReliableUdpChannel::CheckResends() {
  auto now = Clock::now();

  if (!pending_fragments_.empty()) {
    CleanupStaleFragments();
  }

  bool had_timeout = false;
  bool dead_link = false;
  for (auto& [seq, pkt] : unacked_) {
    if (now - pkt.first_sent_at >= kDeadLinkTimeout) {
      dead_link = true;
      break;
    }

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

  if (dead_link) {
    ATLAS_LOG_WARNING(
        "Dead link to {}: oldest unacked packet age >= {}ms, condemning channel",
        remote_.ToString(),
        std::chrono::duration_cast<std::chrono::milliseconds>(kDeadLinkTimeout).count());
    remote_failed_ = true;
    OnDisconnect();
    return;
  }

  if (had_timeout) {
    OnLossCwndUpdate(true);
  }
}

void ReliableUdpChannel::OnAckCwndUpdate(uint32_t acked_count) {
  if (nocwnd_) {
    return;
  }

  const auto mss = static_cast<uint32_t>(mtu_);

  for (uint32_t i = 0; i < acked_count; ++i) {
    if (cwnd_ < ssthresh_) {
      cwnd_++;
      cwnd_incr_ += mss;
    } else {
      if (cwnd_incr_ == 0) {
        cwnd_incr_ = mss;
      }
      cwnd_incr_ += (mss * mss / cwnd_incr_ + mss / 16);

      auto new_cwnd = cwnd_incr_ / mss;
      if (new_cwnd > cwnd_) {
        cwnd_ = new_cwnd;
      }
    }
  }

  if (cwnd_ > send_window_) {
    cwnd_ = send_window_;
    cwnd_incr_ = send_window_ * mss;
  }
}

void ReliableUdpChannel::OnLossCwndUpdate(bool is_timeout) {
  if (nocwnd_) {
    return;
  }

  auto in_flight = static_cast<uint32_t>(unacked_.size());
  const auto mss = static_cast<uint32_t>(mtu_);

  if (is_timeout) {
    ssthresh_ = std::max(cwnd_ / 2, 2u);
    cwnd_ = 1;
    cwnd_incr_ = mss;
  } else {
    ssthresh_ = std::max(in_flight / 2, 2u);
    cwnd_ = ssthresh_ + fast_resend_thresh_;
    cwnd_incr_ = cwnd_ * mss;
  }
}

void ReliableUdpChannel::StartResendTimer() {
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
