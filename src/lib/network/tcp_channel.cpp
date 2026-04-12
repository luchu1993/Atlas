#include "network/tcp_channel.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"

#include <array>
#include <cstring>

namespace atlas
{

TcpChannel::TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket socket,
                       const Address& remote)
    : Channel(dispatcher, table, remote), socket_(std::move(socket))
{
}

TcpChannel::~TcpChannel()
{
    cancel_recv_buffer_shrink();
    cancel_write_buffer_shrink();

    if (socket_.is_valid())
    {
        (void)dispatcher_.deregister(socket_.fd());
        socket_.close();
    }
}

// ============================================================================
// Receive path
// ============================================================================

void TcpChannel::on_readable()
{
    std::size_t reads = 0;
    while (true)
    {
        if (reads >= kMaxSocketReadsPerCallback)
        {
            break;
        }

        auto span = recv_buffer_.writable_span();
        if (span.empty())
        {
            if (!ensure_recv_writable())
            {
                ATLAS_LOG_ERROR("TcpChannel recv buffer full from {}", remote_.to_string());
                on_disconnect();
                return;
            }
            span = recv_buffer_.writable_span();
        }

        auto result = socket_.recv(span);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                break;
            }
            ATLAS_LOG_WARNING("TcpChannel recv error from {}: {}", remote_.to_string(),
                              result.error().message());
            on_disconnect();
            return;
        }

        if (*result == 0)
        {
            on_disconnect();
            return;
        }

        cancel_recv_buffer_shrink();
        recv_buffer_.commit(*result);
        on_data_received(std::span<const std::byte>(span.data(), *result));
        ++reads;
    }

    process_recv_buffer();
}

auto TcpChannel::ensure_recv_writable() -> bool
{
    std::size_t needed = 1;

    if (recv_buffer_.readable_size() >= kFrameHeaderSize)
    {
        auto frame_length = peek_frame_length();
        if (!frame_length)
        {
            return false;
        }
        if (*frame_length > kMaxBundleSize)
        {
            ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}", *frame_length,
                            remote_.to_string());
            return false;
        }

        auto total = kFrameHeaderSize + static_cast<std::size_t>(*frame_length);
        if (total > recv_buffer_.readable_size())
        {
            needed = total - recv_buffer_.readable_size();
        }
    }

    return recv_buffer_.ensure_writable(needed);
}

auto TcpChannel::peek_frame_length() const -> std::optional<uint32_t>
{
    if (recv_buffer_.readable_size() < kFrameHeaderSize)
    {
        return std::nullopt;
    }

    std::array<std::byte, kFrameHeaderSize> header{};
    if (!recv_buffer_.peek_front(header))
    {
        return std::nullopt;
    }

    uint32_t frame_length = 0;
    std::memcpy(&frame_length, header.data(), sizeof(frame_length));
    return endian::from_little(frame_length);
}

void TcpChannel::process_recv_buffer()
{
    while (true)
    {
        auto available = recv_buffer_.readable_size();

        if (available < kFrameHeaderSize)
        {
            break;
        }

        auto frame_length = peek_frame_length();
        if (!frame_length)
        {
            break;
        }

        if (*frame_length > kMaxBundleSize)
        {
            ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}", *frame_length,
                            remote_.to_string());
            on_disconnect();
            return;
        }

        std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(*frame_length);
        if (available < total)
        {
            auto remaining = total - available;
            if (!recv_buffer_.ensure_writable(remaining))
            {
                ATLAS_LOG_ERROR("TcpChannel cannot grow recv buffer to {} bytes for {}", total,
                                remote_.to_string());
                on_disconnect();
                return;
            }
            break;
        }

        auto rspan = recv_buffer_.readable_span();
        // Ensure contiguous data for dispatch
        if (rspan.size() < total)
        {
            recv_buffer_.linearize();
            rspan = recv_buffer_.readable_span();
        }

        const std::byte* frame_start = rspan.data() + kFrameHeaderSize;

        if (packet_filter_)
        {
            auto filtered = packet_filter_->recv_filter(
                std::span<const std::byte>(frame_start, static_cast<std::size_t>(*frame_length)));
            if (filtered)
            {
                dispatch_messages(std::span<const std::byte>(filtered->data(), filtered->size()));
            }
            else
            {
                ATLAS_LOG_WARNING("TcpChannel recv_filter failed from {}: {}", remote_.to_string(),
                                  filtered.error().message());
            }
        }
        else
        {
            dispatch_messages(
                std::span<const std::byte>(frame_start, static_cast<std::size_t>(*frame_length)));
        }

        recv_buffer_.consume(total);
    }

    if (recv_buffer_.readable_size() == 0)
    {
        schedule_recv_buffer_shrink();
    }
    else
    {
        cancel_recv_buffer_shrink();
    }
}

// ============================================================================
// Write path
// ============================================================================

void TcpChannel::on_writable()
{
    try_flush_write_buffer();
}

auto TcpChannel::do_send(std::span<const std::byte> data) -> Result<size_t>
{
    uint32_t frame_length = endian::to_little(static_cast<uint32_t>(data.size()));
    auto header = std::span<const std::byte>(reinterpret_cast<const std::byte*>(&frame_length),
                                             sizeof(uint32_t));

    if (!write_buffer_.ensure_writable(header.size() + data.size()))
    {
        return Error(ErrorCode::WouldBlock, "Write buffer full");
    }

    cancel_write_buffer_shrink();
    if (!write_buffer_.append(header) || !write_buffer_.append(data))
    {
        return Error(ErrorCode::IoError, "Write buffer append failed after reserve");
    }

    try_flush_write_buffer();
    return static_cast<size_t>(data.size());
}

void TcpChannel::on_condemned()
{
    cancel_recv_buffer_shrink();
    cancel_write_buffer_shrink();

    recv_buffer_.clear();
    recv_buffer_.shrink_to_fit();

    write_buffer_.clear();
    write_buffer_.shrink_to_fit();

    write_registered_ = false;
}

void TcpChannel::try_flush_write_buffer(std::size_t write_budget)
{
    std::size_t flushes = 0;
    while (write_buffer_.readable_size() > 0)
    {
        if (flushes >= write_budget)
        {
            update_write_interest();
            cancel_write_buffer_shrink();
            return;
        }

        auto rspan = write_buffer_.readable_span();
        auto result = socket_.send(rspan);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                update_write_interest();
                return;
            }
            ATLAS_LOG_WARNING("TcpChannel send error to {}: {}", remote_.to_string(),
                              result.error().message());
            on_disconnect();
            return;
        }

        write_buffer_.consume(*result);
        ++flushes;
    }

    if (write_registered_)
    {
        update_write_interest();
    }

    if (write_buffer_.readable_size() == 0)
    {
        schedule_write_buffer_shrink();
    }
    else
    {
        cancel_write_buffer_shrink();
    }
}

void TcpChannel::update_write_interest()
{
    bool need_write = write_buffer_.readable_size() > 0;
    if (need_write && !write_registered_)
    {
        auto interest = IOEvent::Readable | IOEvent::Writable;
        auto result = dispatcher_.modify_interest(socket_.fd(), interest);
        if (!result)
        {
            ATLAS_LOG_ERROR("Failed to set write interest for {}: {}", remote_.to_string(),
                            result.error().message());
            on_disconnect();
            return;
        }
        write_registered_ = true;
    }
    else if (!need_write && write_registered_)
    {
        auto result = dispatcher_.modify_interest(socket_.fd(), IOEvent::Readable);
        if (!result)
        {
            ATLAS_LOG_ERROR("Failed to clear write interest for {}: {}", remote_.to_string(),
                            result.error().message());
            on_disconnect();
            return;
        }
        write_registered_ = false;
    }
}

void TcpChannel::schedule_recv_buffer_shrink()
{
    if (recv_buffer_.capacity() <= recv_buffer_.min_capacity() ||
        recv_buffer_shrink_timer_.is_valid())
    {
        return;
    }

    recv_buffer_shrink_timer_ = dispatcher_.add_timer(
        kBufferShrinkDelay, [this](TimerHandle handle) { on_recv_buffer_shrink(handle); });
}

void TcpChannel::cancel_recv_buffer_shrink()
{
    if (recv_buffer_shrink_timer_.is_valid())
    {
        dispatcher_.cancel_timer(recv_buffer_shrink_timer_);
        recv_buffer_shrink_timer_ = TimerHandle{};
    }
}

void TcpChannel::on_recv_buffer_shrink(TimerHandle handle)
{
    if (recv_buffer_shrink_timer_ != handle)
    {
        return;
    }

    recv_buffer_shrink_timer_ = TimerHandle{};
    if (recv_buffer_.readable_size() == 0)
    {
        recv_buffer_.shrink_to_fit();
    }
}

void TcpChannel::schedule_write_buffer_shrink()
{
    if (write_buffer_.capacity() <= write_buffer_.min_capacity() ||
        write_buffer_shrink_timer_.is_valid())
    {
        return;
    }

    write_buffer_shrink_timer_ = dispatcher_.add_timer(
        kBufferShrinkDelay, [this](TimerHandle handle) { on_write_buffer_shrink(handle); });
}

void TcpChannel::cancel_write_buffer_shrink()
{
    if (write_buffer_shrink_timer_.is_valid())
    {
        dispatcher_.cancel_timer(write_buffer_shrink_timer_);
        write_buffer_shrink_timer_ = TimerHandle{};
    }
}

void TcpChannel::on_write_buffer_shrink(TimerHandle handle)
{
    if (write_buffer_shrink_timer_ != handle)
    {
        return;
    }

    write_buffer_shrink_timer_ = TimerHandle{};
    if (write_buffer_.readable_size() == 0)
    {
        write_buffer_.shrink_to_fit();
    }
}

}  // namespace atlas
