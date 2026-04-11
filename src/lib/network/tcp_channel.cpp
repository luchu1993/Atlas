#include "network/tcp_channel.hpp"

#include "foundation/log.hpp"
#include "network/event_dispatcher.hpp"

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
    while (true)
    {
        auto span = recv_buffer_.writable_span();
        if (span.empty())
        {
            ATLAS_LOG_ERROR("TcpChannel recv buffer full from {}", remote_.to_string());
            on_disconnect();
            return;
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

        recv_buffer_.commit(*result);
        on_data_received(std::span<const std::byte>(span.data(), *result));
    }

    process_recv_buffer();
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

        // Peek frame length — may span the ring buffer wrap point
        auto rspan = recv_buffer_.readable_span();
        uint32_t frame_length;

        if (rspan.size() >= kFrameHeaderSize)
        {
            std::memcpy(&frame_length, rspan.data(), sizeof(uint32_t));
        }
        else
        {
            recv_buffer_.linearize();
            rspan = recv_buffer_.readable_span();
            std::memcpy(&frame_length, rspan.data(), sizeof(uint32_t));
        }

        frame_length = endian::from_little(frame_length);

        if (frame_length > kMaxBundleSize)
        {
            ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}", frame_length,
                            remote_.to_string());
            on_disconnect();
            return;
        }

        std::size_t total = kFrameHeaderSize + frame_length;
        if (available < total)
        {
            break;
        }

        // Ensure contiguous data for dispatch
        if (rspan.size() < total)
        {
            recv_buffer_.linearize();
            rspan = recv_buffer_.readable_span();
        }

        const std::byte* frame_start = rspan.data() + kFrameHeaderSize;

        if (packet_filter_)
        {
            auto filtered =
                packet_filter_->recv_filter(std::span<const std::byte>(frame_start, frame_length));
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
            dispatch_messages(std::span<const std::byte>(frame_start, frame_length));
        }

        recv_buffer_.consume(total);
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

    if (!write_buffer_.append(header) || !write_buffer_.append(data))
    {
        return Error(ErrorCode::WouldBlock, "Write buffer full");
    }

    try_flush_write_buffer();
    return static_cast<size_t>(data.size());
}

void TcpChannel::try_flush_write_buffer()
{
    while (write_buffer_.readable_size() > 0)
    {
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
    }

    if (write_registered_)
    {
        update_write_interest();
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

}  // namespace atlas
