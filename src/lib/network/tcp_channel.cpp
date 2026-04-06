#include "network/tcp_channel.hpp"
#include "network/event_dispatcher.hpp"
#include "foundation/log.hpp"

#include <array>
#include <cstring>

namespace atlas
{

TcpChannel::TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                       Socket socket, const Address& remote)
    : Channel(dispatcher, table, remote)
    , socket_(std::move(socket))
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

void TcpChannel::on_readable()
{
    std::array<std::byte, 8192> temp{};
    while (true)
    {
        auto result = socket_.recv(temp);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                break;  // no more data available
            }
            ATLAS_LOG_WARNING("TcpChannel recv error from {}: {}",
                remote_.to_string(), result.error().message());
            on_disconnect();
            return;
        }

        if (*result == 0)
        {
            // Peer closed connection (orderly shutdown)
            on_disconnect();
            return;
        }

        recv_buffer_.insert(recv_buffer_.end(), temp.data(), temp.data() + *result);
        on_data_received(std::span<const std::byte>(temp.data(), *result));

        // Backpressure: if recv buffer too large, condemn
        if (recv_buffer_.size() > kMaxRecvBufferSize)
        {
            ATLAS_LOG_ERROR("TcpChannel recv buffer overflow from {}",
                remote_.to_string());
            on_disconnect();
            return;
        }
    }

    process_recv_buffer();
}

void TcpChannel::process_recv_buffer()
{
    while (true)
    {
        // Need at least frame header (4 bytes)
        if (recv_buffer_.size() < kFrameHeaderSize)
        {
            break;
        }

        // Read frame length (first 4 bytes, little-endian)
        uint32_t frame_length;
        std::memcpy(&frame_length, recv_buffer_.data(), sizeof(uint32_t));
        frame_length = endian::from_little(frame_length);

        // Sanity check
        if (frame_length > kMaxBundleSize)
        {
            ATLAS_LOG_ERROR("TcpChannel oversized frame {} from {}",
                frame_length, remote_.to_string());
            on_disconnect();
            return;
        }

        // Check if we have the full frame
        std::size_t total = kFrameHeaderSize + frame_length;
        if (recv_buffer_.size() < total)
        {
            break;  // wait for more data
        }

        // Extract frame and dispatch messages
        auto* frame_start = recv_buffer_.data() + kFrameHeaderSize;
        dispatch_messages(std::span<const std::byte>(frame_start, frame_length));

        // Consume from recv buffer
        recv_buffer_.erase(recv_buffer_.begin(),
                           recv_buffer_.begin() + static_cast<std::ptrdiff_t>(total));
    }
}

void TcpChannel::on_writable()
{
    try_flush_write_buffer();
}

auto TcpChannel::do_send(std::span<const std::byte> data) -> Result<size_t>
{
    // Prepend frame header: [uint32 frame_length LE]
    uint32_t frame_length = endian::to_little(static_cast<uint32_t>(data.size()));
    auto* header_bytes = reinterpret_cast<const std::byte*>(&frame_length);

    write_buffer_.insert(write_buffer_.end(), header_bytes,
                         header_bytes + sizeof(uint32_t));
    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    try_flush_write_buffer();

    return static_cast<size_t>(data.size());
}

void TcpChannel::try_flush_write_buffer()
{
    while (!write_buffer_.empty())
    {
        auto result = socket_.send(write_buffer_);
        if (!result)
        {
            if (result.error().code() == ErrorCode::WouldBlock)
            {
                // Register for write events to flush later
                update_write_interest();
                return;
            }
            ATLAS_LOG_WARNING("TcpChannel send error to {}: {}",
                remote_.to_string(), result.error().message());
            on_disconnect();
            return;
        }

        // Erase sent bytes
        write_buffer_.erase(write_buffer_.begin(),
                            write_buffer_.begin() + static_cast<std::ptrdiff_t>(*result));
    }

    // All data flushed - remove write interest if registered
    if (write_registered_)
    {
        update_write_interest();
    }
}

void TcpChannel::update_write_interest()
{
    bool need_write = !write_buffer_.empty();
    if (need_write && !write_registered_)
    {
        auto interest = IOEvent::Readable | IOEvent::Writable;
        auto result = dispatcher_.modify_interest(socket_.fd(), interest);
        if (!result)
        {
            ATLAS_LOG_ERROR("Failed to set write interest for {}: {}",
                remote_.to_string(), result.error().message());
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
            ATLAS_LOG_ERROR("Failed to clear write interest for {}: {}",
                remote_.to_string(), result.error().message());
            on_disconnect();
            return;
        }
        write_registered_ = false;
    }
}

} // namespace atlas
