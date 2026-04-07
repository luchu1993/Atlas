#include "network/reliable_udp.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "serialization/binary_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace atlas
{

// ============================================================================
// Construction / Destruction
// ============================================================================

ReliableUdpChannel::ReliableUdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                                       Socket& shared_socket, const Address& remote)
    : Channel(dispatcher, table, remote), shared_socket_(shared_socket)
{
}

ReliableUdpChannel::~ReliableUdpChannel()
{
    stop_resend_timer();
}

// ============================================================================
// effective_window — min(send_window, cwnd) or just send_window if nocwnd
// ============================================================================

auto ReliableUdpChannel::effective_window() const -> uint32_t
{
    if (nocwnd_)
    {
        return send_window_;
    }
    return std::min(send_window_, cwnd_);
}

// ============================================================================
// send_reliable
// ============================================================================

auto ReliableUdpChannel::send_reliable() -> Result<void>
{
    if (state_ == ChannelState::Condemned)
    {
        return Error(ErrorCode::ChannelCondemned, "Channel is condemned");
    }

    if (bundle_.empty())
    {
        return Result<void>{};
    }

    auto eff_wnd = effective_window();
    if (unacked_.size() >= eff_wnd)
    {
        return Error(ErrorCode::WouldBlock, "Send window full");
    }

    auto payload = bundle_.finalize();

    // Auto-fragment if payload exceeds max UDP payload
    if (payload.size() > rudp::kMaxUdpPayload)
    {
        return send_fragmented(payload);
    }

    auto seq = next_send_seq_++;

    uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
    if (remote_seq_ > 0)
    {
        flags |= rudp::kFlagHasAck;
    }

    auto packet =
        build_packet(flags, seq, std::span<const std::byte>(payload.data(), payload.size()));

    // Store for potential retransmission
    unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

    auto result = shared_socket_.send_to(packet, remote_);
    if (!result)
    {
        return result.error();
    }
    bytes_sent_ += *result;

    start_resend_timer();
    return Result<void>{};
}

// ============================================================================
// send_unreliable
// ============================================================================

auto ReliableUdpChannel::send_unreliable() -> Result<void>
{
    if (state_ == ChannelState::Condemned)
    {
        return Error(ErrorCode::ChannelCondemned, "Channel is condemned");
    }

    if (bundle_.empty())
    {
        return Result<void>{};
    }

    auto payload = bundle_.finalize();

    uint8_t flags = 0;
    if (remote_seq_ > 0)
    {
        flags |= rudp::kFlagHasAck;  // piggyback ACK even on unreliable
    }

    auto packet =
        build_packet(flags, 0, std::span<const std::byte>(payload.data(), payload.size()));

    auto result = shared_socket_.send_to(packet, remote_);
    if (!result)
    {
        return result.error();
    }
    bytes_sent_ += *result;
    return Result<void>{};
}

// ============================================================================
// do_send (Channel virtual — treats raw data as reliable)
// ============================================================================

auto ReliableUdpChannel::do_send(std::span<const std::byte> data) -> Result<size_t>
{
    auto seq = next_send_seq_++;

    uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
    if (remote_seq_ > 0)
    {
        flags |= rudp::kFlagHasAck;
    }

    auto packet = build_packet(flags, seq, data);

    unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

    auto result = shared_socket_.send_to(packet, remote_);
    if (!result)
    {
        return result.error();
    }

    start_resend_timer();
    return *result;
}

// ============================================================================
// build_packet
// ============================================================================

auto ReliableUdpChannel::build_packet(uint8_t flags, SeqNum seq, std::span<const std::byte> payload,
                                      const FragmentHeader* frag) -> std::vector<std::byte>
{
    BinaryWriter header;
    header.write<uint8_t>(flags);

    if (flags & rudp::kFlagHasSeq)
    {
        header.write<uint32_t>(seq);
    }

    if (flags & rudp::kFlagHasAck)
    {
        header.write<uint32_t>(remote_seq_);
        header.write<uint32_t>(recv_ack_bits_);
    }

    if ((flags & rudp::kFlagFragment) && frag)
    {
        header.write<uint16_t>(frag->fragment_id);
        header.write<uint8_t>(frag->fragment_index);
        header.write<uint8_t>(frag->fragment_count);
    }

    auto hdr = header.data();
    std::vector<std::byte> pkt;
    pkt.reserve(hdr.size() + payload.size());
    pkt.insert(pkt.end(), hdr.begin(), hdr.end());
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// ============================================================================
// on_datagram_received
// ============================================================================

void ReliableUdpChannel::on_datagram_received(std::span<const std::byte> data)
{
    if (data.empty())
    {
        return;
    }

    BinaryReader reader(data);

    auto flags_result = reader.read<uint8_t>();
    if (!flags_result)
    {
        return;
    }
    uint8_t flags = *flags_result;

    SeqNum seq = 0;
    if (flags & rudp::kFlagHasSeq)
    {
        auto s = reader.read<uint32_t>();
        if (!s)
        {
            return;
        }
        seq = *s;
    }

    if (flags & rudp::kFlagHasAck)
    {
        auto ack_num = reader.read<uint32_t>();
        auto ack_bits = reader.read<uint32_t>();
        if (!ack_num || !ack_bits)
        {
            return;
        }
        process_ack(*ack_num, *ack_bits);
    }

    // Read fragment header if present
    FragmentHeader frag_hdr{};
    bool is_fragment = (flags & rudp::kFlagFragment) != 0;
    if (is_fragment)
    {
        auto fid = reader.read<uint16_t>();
        auto fidx = reader.read<uint8_t>();
        auto fcnt = reader.read<uint8_t>();
        if (!fid || !fidx || !fcnt)
        {
            return;
        }
        frag_hdr = {*fid, *fidx, *fcnt};
    }

    // Remaining data is the payload
    auto remaining = reader.remaining();
    if (remaining == 0)
    {
        return;
    }

    auto payload = reader.read_bytes(remaining);
    if (!payload)
    {
        return;
    }

    if (flags & rudp::kFlagHasSeq)
    {
        // Reliable packet -- check duplicate and receive window
        if (is_duplicate(seq))
        {
            ATLAS_LOG_DEBUG("Duplicate packet seq={} from {}", seq, remote_.to_string());
            return;
        }

        // Receive window check: drop if too far ahead of delivery frontier
        if (seq_greater_than(seq, rcv_nxt_ + rcv_wnd_))
        {
            ATLAS_LOG_DEBUG("Packet seq={} outside receive window (nxt={}, wnd={})", seq, rcv_nxt_,
                            rcv_wnd_);
            return;
        }

        record_received_seq(seq);
        bytes_received_ += remaining;

        // Enqueue for ordered delivery instead of immediate dispatch
        enqueue_for_delivery(seq, *payload, is_fragment, frag_hdr);
        flush_receive_buffer();
    }
    else
    {
        // Unreliable packet — dispatch immediately (no ordering guarantee)
        bytes_received_ += remaining;
        on_data_received(*payload);
        dispatch_messages(*payload);
    }
}

// ============================================================================
// process_ack
// ============================================================================

void ReliableUdpChannel::process_ack(SeqNum ack_num, uint32_t ack_bits)
{
    // --- Phase 1: Process explicit ACKs via direct map lookup ---
    //
    // The ACK window covers ack_num and the 32 packets before it.
    // Instead of iterating all N unacked entries and testing each,
    // iterate the (at most 33) possible ACK'd seq numbers and find()
    // directly in the map — O(33·log N) vs O(N).
    std::vector<SeqNum> acked_seqs;
    auto now = Clock::now();

    // ack_num itself is always ACK'd
    for (int32_t diff = 0; diff <= 32; ++diff)
    {
        SeqNum candidate = ack_num - static_cast<SeqNum>(diff);
        if (diff > 0 && !(ack_bits & (1u << (diff - 1))))
        {
            continue;  // bit not set — not ACK'd
        }

        auto it = unacked_.find(candidate);
        if (it == unacked_.end())
        {
            continue;
        }

        // ACK'd — update RTT with Karn's algorithm (first send only)
        if (it->second.send_count == 1)
        {
            update_rtt(now - it->second.sent_at);
        }
        acked_seqs.push_back(candidate);
        unacked_.erase(it);
    }

    // --- Phase 2: Purge entries too old for the ACK window (O(N)) ---
    // These are sequences more than 32 behind ack_num — considered lost.
    std::erase_if(unacked_, [ack_num](const auto& kv)
                  { return seq_less_than(kv.first, ack_num) && seq_diff(ack_num, kv.first) > 32; });

    // Congestion window growth
    if (!acked_seqs.empty())
    {
        on_ack_cwnd_update(static_cast<uint32_t>(acked_seqs.size()));
    }

    // --- Phase 3: Fast retransmit ---
    // For each remaining unacked packet, if ANY newer packet was just ACK'd
    // (i.e., seq < max(acked_seqs)), increment its skip_count once.
    // Using the max is O(N + |acked|) instead of O(N × |acked|).
    bool fast_resent = false;
    if (fast_resend_thresh_ > 0 && !acked_seqs.empty())
    {
        // Find the newest acked sequence (highest in wrapping sense)
        SeqNum max_acked = acked_seqs[0];
        for (SeqNum s : acked_seqs)
        {
            if (seq_less_than(max_acked, s))
            {
                max_acked = s;
            }
        }

        for (auto& [seq, pkt] : unacked_)
        {
            if (seq_less_than(seq, max_acked))
            {
                pkt.skip_count++;
            }

            if (pkt.skip_count >= fast_resend_thresh_)
            {
                auto result = shared_socket_.send_to(pkt.data, remote_);
                if (result)
                {
                    bytes_sent_ += *result;
                }
                pkt.sent_at = now;
                pkt.send_count++;
                pkt.skip_count = 0;

                ATLAS_LOG_DEBUG("Fast resend seq={} to {}", seq, remote_.to_string());
                fast_resent = true;
            }
        }

        if (fast_resent)
        {
            on_loss_cwnd_update(false);
        }
    }

    if (unacked_.empty())
    {
        stop_resend_timer();
    }
}

// ============================================================================
// update_rtt (Jacobson/Karels per RFC 6298)
// ============================================================================

void ReliableUdpChannel::update_rtt(Duration sample)
{
    double s = std::chrono::duration<double>(sample).count();
    double r = std::chrono::duration<double>(rtt_).count();
    double v = std::chrono::duration<double>(rtt_var_).count();

    if (r < 1e-9)
    {
        r = s;
        v = s / 2.0;
    }
    else
    {
        v = v * 0.75 + std::abs(r - s) * 0.25;
        r = r * 0.875 + s * 0.125;
    }

    double rto = r + v * 4.0;
    double min_rto = nodelay_ ? 0.03 : 0.2;  // 30ms in nodelay, 200ms normal
    rto = std::clamp(rto, min_rto, 5.0);

    rtt_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(r));
    rtt_var_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(v));
    rto_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(rto));
}

// ============================================================================
// record_received_seq
// ============================================================================

void ReliableUdpChannel::record_received_seq(SeqNum seq)
{
    if (seq_greater_than(seq, remote_seq_))
    {
        // New highest -- shift ack_bits
        auto shift = static_cast<uint32_t>(seq - remote_seq_);
        if (shift < 32)
        {
            recv_ack_bits_ <<= shift;
            // Set bit for old remote_seq_ (which is now shift positions back)
            if (remote_seq_ > 0)
            {
                recv_ack_bits_ |= (1u << (shift - 1));
            }
        }
        else
        {
            recv_ack_bits_ = 0;
            if (remote_seq_ > 0 && shift == 32)
            {
                recv_ack_bits_ = (1u << 31);
            }
        }
        remote_seq_ = seq;
    }
    else
    {
        // Older packet -- set bit in ack_bits
        ack_bits_set(recv_ack_bits_, remote_seq_, seq);
    }
}

// ============================================================================
// is_duplicate
// ============================================================================

auto ReliableUdpChannel::is_duplicate(SeqNum seq) const -> bool
{
    if (seq == 0)
    {
        return false;
    }
    if (seq_less_than(seq, remote_seq_))
    {
        // Old packet -- check if in ack_bits window
        auto diff = static_cast<int32_t>(remote_seq_ - seq);
        if (diff > 32)
        {
            return true;  // too old
        }
        return ack_bits_test(recv_ack_bits_, remote_seq_, seq);
    }
    if (seq == remote_seq_)
    {
        return true;  // exact duplicate
    }
    return false;  // new packet
}

// ============================================================================
// enqueue_for_delivery — store in receive buffer for ordered delivery
// ============================================================================

void ReliableUdpChannel::enqueue_for_delivery(SeqNum seq, std::span<const std::byte> payload,
                                              bool is_fragment, const FragmentHeader& frag_hdr)
{
    // Already delivered (seq < rcv_nxt_) — should have been caught by is_duplicate
    if (seq_less_than(seq, rcv_nxt_))
    {
        return;
    }

    // Already in buffer
    if (rcv_buf_.count(seq) != 0)
    {
        return;
    }

    rcv_buf_[seq] =
        RecvEntry{std::vector<std::byte>(payload.begin(), payload.end()), is_fragment, frag_hdr};
}

// ============================================================================
// flush_receive_buffer — deliver consecutive packets starting from rcv_nxt_
// ============================================================================

void ReliableUdpChannel::flush_receive_buffer()
{
    while (true)
    {
        auto it = rcv_buf_.find(rcv_nxt_);
        if (it == rcv_buf_.end())
        {
            break;  // gap — waiting for rcv_nxt_ to arrive
        }

        auto entry = std::move(it->second);
        rcv_buf_.erase(it);
        rcv_nxt_++;

        if (entry.is_fragment)
        {
            // Buffer the fragment for reassembly
            on_fragment_received(entry.frag_hdr, std::span<const std::byte>(entry.payload));
        }
        else
        {
            // Non-fragment: dispatch immediately
            on_data_received(entry.payload);
            dispatch_messages(entry.payload);
        }
    }
}

// ============================================================================
// send_fragmented — split payload into MTU-sized reliable fragments
// ============================================================================

auto ReliableUdpChannel::send_fragmented(std::span<const std::byte> payload) -> Result<void>
{
    auto frag_payload_size = rudp::kMaxUdpPayload;
    auto total = (payload.size() + frag_payload_size - 1) / frag_payload_size;

    if (total > rudp::kMaxFragments)
    {
        return Error(
            ErrorCode::MessageTooLarge,
            std::format("Message too large for fragmentation: {} bytes ({} fragments, max {})",
                        payload.size(), total, rudp::kMaxFragments));
    }

    if (unacked_.size() + total > effective_window())
    {
        return Error(ErrorCode::WouldBlock, "Send window too small for fragmented message");
    }

    auto frag_id = next_fragment_id_++;
    auto frag_count = static_cast<uint8_t>(total);

    for (uint8_t i = 0; i < frag_count; ++i)
    {
        auto offset = static_cast<std::size_t>(i) * frag_payload_size;
        auto chunk_size = std::min(frag_payload_size, payload.size() - offset);
        auto chunk = payload.subspan(offset, chunk_size);

        auto seq = next_send_seq_++;

        uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq | rudp::kFlagFragment;
        if (remote_seq_ > 0)
        {
            flags |= rudp::kFlagHasAck;
        }

        FragmentHeader fhdr{frag_id, i, frag_count};
        auto packet = build_packet(flags, seq, chunk, &fhdr);

        unacked_[seq] = UnackedPacket{packet, Clock::now(), 1};

        auto result = shared_socket_.send_to(packet, remote_);
        if (!result)
        {
            return result.error();
        }
        bytes_sent_ += *result;
    }

    start_resend_timer();
    return Result<void>{};
}

// ============================================================================
// on_fragment_received — buffer and reassemble fragments
// ============================================================================

void ReliableUdpChannel::on_fragment_received(const FragmentHeader& hdr,
                                              std::span<const std::byte> payload)
{
    if (hdr.fragment_count == 0 || hdr.fragment_index >= hdr.fragment_count)
    {
        ATLAS_LOG_WARNING("Invalid fragment header from {}", remote_.to_string());
        return;
    }

    auto& group = pending_fragments_[hdr.fragment_id];

    // Initialize group on first fragment
    if (group.expected_count == 0)
    {
        group.expected_count = hdr.fragment_count;
        group.fragments.resize(hdr.fragment_count);
        group.first_received = Clock::now();

        // Memory protection: limit concurrent fragment groups
        if (pending_fragments_.size() > kMaxPendingFragmentGroups)
        {
            cleanup_stale_fragments();
        }
    }
    else if (group.expected_count != hdr.fragment_count)
    {
        ATLAS_LOG_WARNING("Fragment count mismatch for id={}", hdr.fragment_id);
        return;
    }

    // Store fragment (skip if already received — duplicate)
    if (group.fragments[hdr.fragment_index].empty())
    {
        group.fragments[hdr.fragment_index].assign(payload.begin(), payload.end());
        group.received_count++;
    }

    // Check if all fragments received
    if (group.received_count == group.expected_count)
    {
        // Reassemble
        std::vector<std::byte> full_payload;
        std::size_t total_size = 0;
        for (const auto& frag : group.fragments)
        {
            total_size += frag.size();
        }
        full_payload.reserve(total_size);
        for (const auto& frag : group.fragments)
        {
            full_payload.insert(full_payload.end(), frag.begin(), frag.end());
        }

        pending_fragments_.erase(hdr.fragment_id);

        // Dispatch the reassembled message
        on_data_received(full_payload);
        dispatch_messages(full_payload);
    }
}

// ============================================================================
// cleanup_stale_fragments — remove incomplete groups older than timeout
// ============================================================================

void ReliableUdpChannel::cleanup_stale_fragments()
{
    auto now = Clock::now();
    std::erase_if(pending_fragments_, [now](const auto& pair)
                  { return (now - pair.second.first_received) >= rudp::kFragmentTimeout; });
}

// ============================================================================
// check_resends
// ============================================================================

void ReliableUdpChannel::check_resends()
{
    auto now = Clock::now();

    // Periodically clean up stale fragment groups
    if (!pending_fragments_.empty())
    {
        cleanup_stale_fragments();
    }

    for (auto& [seq, pkt] : unacked_)
    {
        // Backoff: nodelay uses 1.5x (KCP-style), normal uses 2x
        // Capped at 5 doublings (~30s max)
        auto backoff = rto_;
        for (uint32_t i = 1; i < pkt.send_count && i < 5; ++i)
        {
            if (nodelay_)
            {
                backoff = backoff * 3 / 2;  // 1.5x
            }
            else
            {
                backoff *= 2;
            }
        }

        if (now - pkt.sent_at >= backoff)
        {
            auto result = shared_socket_.send_to(pkt.data, remote_);
            if (result)
            {
                bytes_sent_ += *result;
            }
            pkt.sent_at = now;
            pkt.send_count++;

            // RTO timeout → aggressive cwnd reduction
            on_loss_cwnd_update(true);

            ATLAS_LOG_DEBUG("Resend seq={} attempt={} to {}", seq, pkt.send_count,
                            remote_.to_string());
        }
    }
}

// ============================================================================
// Congestion control (KCP-style: slow start + congestion avoidance)
// ============================================================================

void ReliableUdpChannel::on_ack_cwnd_update(uint32_t acked_count)
{
    if (nocwnd_)
    {
        return;
    }

    for (uint32_t i = 0; i < acked_count; ++i)
    {
        if (cwnd_ < ssthresh_)
        {
            // Slow start: cwnd grows by 1 per ACK (doubles per RTT)
            cwnd_++;
            cwnd_incr_ += rudp::kMtu;
        }
        else
        {
            // Congestion avoidance (KCP formula):
            // incr += mss * mss / incr + mss / 16
            if (cwnd_incr_ == 0)
            {
                cwnd_incr_ = rudp::kMtu;
            }
            cwnd_incr_ +=
                static_cast<uint32_t>(rudp::kMtu * rudp::kMtu / cwnd_incr_ + rudp::kMtu / 16);

            // cwnd = incr / mss
            auto new_cwnd = cwnd_incr_ / static_cast<uint32_t>(rudp::kMtu);
            if (new_cwnd > cwnd_)
            {
                cwnd_ = new_cwnd;
            }
        }
    }

    // Cap at send_window_
    if (cwnd_ > send_window_)
    {
        cwnd_ = send_window_;
        cwnd_incr_ = send_window_ * static_cast<uint32_t>(rudp::kMtu);
    }
}

void ReliableUdpChannel::on_loss_cwnd_update(bool is_timeout)
{
    if (nocwnd_)
    {
        return;
    }

    auto in_flight = static_cast<uint32_t>(unacked_.size());

    if (is_timeout)
    {
        // RTO timeout: aggressive reduction (TCP Tahoe style)
        ssthresh_ = std::max(cwnd_ / 2, 2u);
        cwnd_ = 1;
        cwnd_incr_ = rudp::kMtu;
    }
    else
    {
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

void ReliableUdpChannel::start_resend_timer()
{
    if (resend_timer_.is_valid())
    {
        return;  // already running
    }

    auto min_interval = nodelay_ ? Milliseconds(10) : Milliseconds(50);
    auto interval = std::max(rto_, Duration(min_interval));
    resend_timer_ =
        dispatcher_.add_repeating_timer(interval, [this](TimerHandle) { check_resends(); });
}

void ReliableUdpChannel::stop_resend_timer()
{
    if (resend_timer_.is_valid())
    {
        dispatcher_.cancel_timer(resend_timer_);
        resend_timer_ = TimerHandle{};
    }
}

}  // namespace atlas
