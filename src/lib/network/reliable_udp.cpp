#include "network/reliable_udp.hpp"
#include "network/event_dispatcher.hpp"
#include "network/interface_table.hpp"
#include "serialization/binary_stream.hpp"
#include "foundation/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace atlas
{

// ============================================================================
// Construction / Destruction
// ============================================================================

ReliableUdpChannel::ReliableUdpChannel(EventDispatcher& dispatcher,
                                       InterfaceTable& table,
                                       Socket& shared_socket,
                                       const Address& remote)
    : Channel(dispatcher, table, remote)
    , shared_socket_(shared_socket)
{
}

ReliableUdpChannel::~ReliableUdpChannel()
{
    stop_resend_timer();
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

    if (unacked_.size() >= send_window_)
    {
        return Error(ErrorCode::WouldBlock, "Send window full");
    }

    auto seq = next_send_seq_++;
    auto payload = bundle_.finalize();

    uint8_t flags = rudp::kFlagReliable | rudp::kFlagHasSeq;
    if (remote_seq_ > 0)
    {
        flags |= rudp::kFlagHasAck;
    }

    auto packet = build_packet(flags, seq,
        std::span<const std::byte>(payload.data(), payload.size()));

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

    auto packet = build_packet(flags, 0,
        std::span<const std::byte>(payload.data(), payload.size()));

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

auto ReliableUdpChannel::build_packet(uint8_t flags, SeqNum seq,
                                      std::span<const std::byte> payload)
    -> std::vector<std::byte>
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

    // Remaining data is the bundle payload
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
        // Reliable packet -- check duplicate
        if (is_duplicate(seq))
        {
            ATLAS_LOG_DEBUG("Duplicate packet seq={} from {}",
                seq, remote_.to_string());
            return;
        }
        record_received_seq(seq);
    }

    bytes_received_ += remaining;
    on_data_received(*payload);
    dispatch_messages(*payload);
}

// ============================================================================
// process_ack
// ============================================================================

void ReliableUdpChannel::process_ack(SeqNum ack_num, uint32_t ack_bits)
{
    // Collect ACK'd sequence numbers first
    std::vector<SeqNum> acked_seqs;

    auto it = unacked_.begin();
    while (it != unacked_.end())
    {
        if (ack_bits_test(ack_bits, ack_num, it->first))
        {
            // ACK'd -- update RTT if first send (Karn's algorithm)
            if (it->second.send_count == 1)
            {
                auto sample = Clock::now() - it->second.sent_at;
                update_rtt(sample);
            }
            acked_seqs.push_back(it->first);
            it = unacked_.erase(it);
        }
        else if (seq_less_than(it->first, ack_num) &&
                 seq_diff(ack_num, it->first) > 32)
        {
            // Too old, outside ack_bits window -- consider lost
            it = unacked_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Fast retransmit: for remaining unacked packets, if a LATER packet
    // was just ACK'd, increment the skip count. When skip_count reaches
    // the threshold, retransmit immediately without waiting for RTO.
    if (fast_resend_thresh_ > 0 && !acked_seqs.empty())
    {
        for (auto& [seq, pkt] : unacked_)
        {
            for (auto acked : acked_seqs)
            {
                if (seq_less_than(seq, acked))
                {
                    pkt.skip_count++;
                    break;  // only count once per ACK batch
                }
            }

            if (pkt.skip_count >= fast_resend_thresh_)
            {
                // Fast retransmit!
                auto result = shared_socket_.send_to(pkt.data, remote_);
                if (result)
                {
                    bytes_sent_ += *result;
                }
                pkt.sent_at = Clock::now();
                pkt.send_count++;
                pkt.skip_count = 0;

                ATLAS_LOG_DEBUG("Fast resend seq={} to {}", seq, remote_.to_string());
            }
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

    rtt_ = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(r));
    rtt_var_ = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(v));
    rto_ = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(rto));
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
// check_resends
// ============================================================================

void ReliableUdpChannel::check_resends()
{
    auto now = Clock::now();

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

            ATLAS_LOG_DEBUG("Resend seq={} attempt={} to {}",
                seq, pkt.send_count, remote_.to_string());
        }
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
    resend_timer_ = dispatcher_.add_repeating_timer(
        interval,
        [this](TimerHandle) { check_resends(); });
}

void ReliableUdpChannel::stop_resend_timer()
{
    if (resend_timer_.is_valid())
    {
        dispatcher_.cancel_timer(resend_timer_);
        resend_timer_ = TimerHandle{};
    }
}

} // namespace atlas
