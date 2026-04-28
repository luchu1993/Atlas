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
// send_reliable / send_unreliable — public single-shot wrappers.
//
// SendReliable / SendUnreliable drain the inherited bundle_ (the
// per-channel buffer Channel::SendMessage<Msg> writes into). The
// real work lives in SendBundle{Reliable,Unreliable} which take the
// source bundle as an out-parameter, so FlushDeferred can reuse the
// same packet-building path for the deferred bundles without
// duplicating the seq-allocation / unacked-tracking logic.
// ============================================================================

auto ReliableUdpChannel::SendReliable() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    // Drop staged messages so a caller holding a zombie pointer can't
    // keep growing bundle_ unboundedly — mirrors Channel::Send and
    // SendUnreliable.
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

// ============================================================================
// flush_deferred — drain both BufferMessageDeferred bundles in one call.
// ============================================================================

auto ReliableUdpChannel::FlushDeferred() -> Result<void> {
  if (state_ == ChannelState::kCondemned) {
    deferred_reliable_bundle_.Clear();
    deferred_unreliable_bundle_.Clear();
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

  // Send unreliable first — small, header-only, can't block on cwnd —
  // so a transient kWouldBlock on the reliable side doesn't strand the
  // unreliable batch. Surface the first error encountered.
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

// ============================================================================
// send_bundle_reliable / send_bundle_unreliable — internal helpers
// ============================================================================

auto ReliableUdpChannel::SendBundleReliable(class Bundle& b) -> Result<void> {
  if (b.empty()) {
    return Result<void>{};
  }

  auto eff_wnd = EffectiveWindow();
  if (unacked_.size() >= eff_wnd) {
    return Error(ErrorCode::kWouldBlock, "Send window full");
  }

  auto payload = b.Finalize();

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

  // Store for potential retransmission. Move into the slot before SendTo so
  // we keep a single heap allocation, then send a span over the stored bytes.
  const auto now = Clock::now();
  auto& slot = unacked_[seq];
  slot = UnackedPacket{std::move(packet), now, now, 1};

  auto result = shared_socket_.SendTo(slot.data, remote_);
  if (!result) {
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
  if (state_ == ChannelState::kCondemned) {
    return Error(ErrorCode::kChannelCondemned, "Channel is condemned");
  }

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

  const auto now = Clock::now();
  auto& slot = unacked_[seq];
  slot = UnackedPacket{std::move(packet), now, now, 1};

  auto result = shared_socket_.SendTo(slot.data, remote_);
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
  deferred_reliable_bundle_.Clear();
  deferred_unreliable_bundle_.Clear();
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

// ============================================================================
// on_datagram_received
// ============================================================================

void ReliableUdpChannel::OnDatagramReceived(std::span<const std::byte> data,
                                            TimePoint flush_deadline) {
  if (data.empty()) {
    return;
  }

  // Inbound datagrams — including pure-ACK packets with no payload —
  // are evidence of peer liveness. Channel::OnDataReceived only fires
  // for application payloads, so without this bump a server that pushes
  // data while the client only ACKs back would trip its 10s inactivity
  // timeout (see baseapp.cc:194 / loginapp.cc:126) and disconnect AFK
  // clients. Reset before drop_window so injected drops still register
  // as activity for the inactivity logic.
  ResetInactivityTimer();

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
    auto una = reader.Read<uint32_t>();
    if (!ack_num || !ack_bits || !una) {
      return;
    }
    // Cumulative ACK first — clears anything before the receiver's
    // delivery frontier in one shot, regardless of SACK bitmap reach.
    auto una_acked = ProcessUna(*una);
    if (una_acked != 0) {
      OnAckCwndUpdate(una_acked);
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
      }
      // Always schedule an ACK on a reliable packet, even duplicates —
      // ensures the sender's una marker keeps advancing if its earlier
      // ACK was lost.
      ScheduleDelayedAck();
    }
    return;
  }

  auto payload = reader.ReadBytes(remaining);
  if (!payload) {
    return;
  }

  if (flags & rudp::kFlagHasSeq) {
    // Reliable packet -- check duplicate and receive window.
    // For both rejection paths we still schedule a delayed ACK so the
    // sender's next ACK packet carries our current rcv_nxt_ (una) and
    // any cumulative-stranded retransmits can drain. Without this, a
    // lost ACK could leave the sender retrying packets the receiver
    // already delivered — they hit IsDuplicate, get dropped silently,
    // and the channel eventually trips dead-link condemn.
    if (IsDuplicate(seq)) {
      ATLAS_LOG_DEBUG("Duplicate packet seq={} from {}", seq, remote_.ToString());
      ScheduleDelayedAck();
      return;
    }

    // Receive window check: drop if too far ahead of delivery frontier
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
      // NetworkInterface tracks us as "needs more flushing" and revisits
      // either later in this OnRudpReadable callback or on the next tick.
      hot_cb_(*this);
    }

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
// process_una — KCP-style cumulative ACK. Erases everything strictly below
// the receiver's rcv_nxt_ (una) frontier in one pass, freeing the sender from
// the 32-bit ack_bits SACK reach. Returns the count cleared so the caller
// can roll the increment into a single OnAckCwndUpdate pass alongside the
// SACK-cleared count from ProcessAck.
// ============================================================================

auto ReliableUdpChannel::ProcessUna(SeqNum una) -> uint32_t {
  uint32_t cleared = 0;
  // Take a single RTT sample from the highest cleared first-time-sent
  // packet (Karn's algorithm: never sample retransmits). std::map iterates
  // ascending, so the last qualifying entry seen is the freshest sample.
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

// ============================================================================
// process_ack
// ============================================================================

void ReliableUdpChannel::ProcessAck(SeqNum ack_num, uint32_t ack_bits) {
  std::array<SeqNum, 33> acked_seqs{};
  uint32_t acked_count = 0;
  auto now = Clock::now();

  // Take at most one RTT sample per ACK — and only from the highest
  // acked seq (diff == 0). Updating the smoothed RTT estimator 33 times
  // in a tight loop with samples from the same delayed-ACK batch
  // collapses (rtt*7 + s)/8 into ~98% the latest sample, wiping
  // historical SRTT. KCP samples once per ACK by design (uses the ts
  // field); we approximate by taking the freshest seq's sample.
  // ProcessUna already covers the cumulative-cleared range, so this
  // path only needs to sample when there's a gap (rcv_nxt_ < ack_num).
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
// is_duplicate — delivery-state based, NOT remote_seq_/ack_bits based.
//
// remote_seq_ + ack_bits is a 32-wide SACK reporting window: it tracks
// "what we received recently for the sender's benefit". It is NOT a
// duplicate-detection window. In a gappy stream (e.g. seq 1..49 lost,
// 50..100 arrived) ack_bits only spans seq 68..100 while seq 50..67 have
// fallen off the SACK window despite still sitting in rcv_buf_. Worse,
// seq 1..49 have NEVER been received — yet a remote_seq_-based "too old"
// check (diff > 32) would mark a retransmit of seq 1 as duplicate and
// drop the only gap-filler that could ever advance rcv_nxt_, eventually
// tripping dead-link condemn.
//
// True duplicate-detection state is the delivery frontier: rcv_nxt_
// (everything below has been dispatched) plus rcv_buf_ (everything
// already buffered awaiting in-order flush). KCP draws the line the same
// way.
// ============================================================================

auto ReliableUdpChannel::IsDuplicate(SeqNum seq) const -> bool {
  if (SeqLessThan(seq, rcv_nxt_)) {
    return true;  // already delivered
  }
  if (rcv_buf_.count(seq) != 0) {
    return true;  // already buffered awaiting in-order flush
  }
  return false;
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

auto ReliableUdpChannel::FlushReceiveBuffer(TimePoint deadline) -> bool {
  while (true) {
    auto it = rcv_buf_.find(rcv_nxt_);
    if (it == rcv_buf_.end()) {
      return false;  // gap — waiting for rcv_nxt_ to arrive
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

    // Yield-point: the deadline is checked AFTER each delivery, not
    // before, so we always make progress on at least one packet per
    // call.  Without this, a sufficiently bursty arrival rate combined
    // with a tight deadline could starve the channel even though work
    // is queued.  Stopping here is safe because rcv_nxt_ has already
    // advanced past the just-delivered seq, so a follow-up call resumes
    // exactly where we left off.
    if (Clock::now() >= deadline) {
      return rcv_buf_.find(rcv_nxt_) != rcv_buf_.end();
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

  // Outbound and inbound fragment_id spaces are independent: pending_fragments_
  // tracks the peer's outbound IDs we're reassembling, while next_fragment_id_
  // generates IDs for messages we send. There's no wire-level collision to
  // guard against (each direction has its own datagram stream), so just take
  // the next sequential id.
  auto frag_id = next_fragment_id_++;
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

    const auto now = Clock::now();
    auto& slot = unacked_[seq];
    slot = UnackedPacket{std::move(packet), now, now, 1};

    auto result = shared_socket_.SendTo(slot.data, remote_);
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

  auto it = pending_fragments_.find(hdr.fragment_id);
  if (it == pending_fragments_.end()) {
    // Need to allocate a new reassembly slot — enforce the cap first so
    // a peer can't fan out unique fragment_ids to balloon state. Try
    // CleanupStaleFragments to harvest >30s-old groups, then LRU-evict
    // the oldest if we're still over the cap (cleanup is no-op when
    // every group is fresh, which is exactly the attack pattern).
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
    // buffer left empty — grown on demand by the per-fragment resize
    // below. Pre-reserving fragment_count*MTU (~371 KB) on the first
    // fragment was wasteful when most fragments never arrive.
    group_init.frag_sizes.resize(hdr.fragment_count, 0);
    group_init.first_received = Clock::now();
  } else if (it->second.expected_count != hdr.fragment_count) {
    ATLAS_LOG_WARNING("Fragment count mismatch for id={}", hdr.fragment_id);
    return;
  }
  auto& group = it->second;

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

  // Skip old ACK fields (ack_num + ack_bits + una). The new packet will
  // get fresh ACK info from BuildPacket using current remote_seq_ /
  // recv_ack_bits_ / rcv_nxt_.
  if (flags & rudp::kFlagHasAck) {
    reader.Skip(sizeof(uint32_t) * 3);
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

  // Condemn AFTER the loop so the iterator we just held is no longer in use
  // when OnCondemned() clears unacked_.
  //
  // Use OnDisconnect (not Condemn directly) so the disconnect_callback_ runs
  // and NetworkInterface evicts the channel from channels_. A bare Condemn()
  // leaves a zombie entry that ConnectRudp* will hand back to callers; their
  // next SendMessage then writes into bundle_ and Send() bails on the
  // kCondemned check, leaking the bundle's buffer until the channel
  // eventually destructs.
  if (dead_link) {
    ATLAS_LOG_WARNING(
        "Dead link to {}: oldest unacked packet age >= {}ms, condemning channel",
        remote_.ToString(),
        std::chrono::duration_cast<std::chrono::milliseconds>(kDeadLinkTimeout).count());
    OnDisconnect();
    return;
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
