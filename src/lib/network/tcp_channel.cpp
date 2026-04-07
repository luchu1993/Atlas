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
    std::array<std::byte, 8192> temp{};
    while (true)
    {
        auto result = socket_.recv(temp);
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

        recv_buffer_.insert(recv_buffer_.end(), temp.data(), temp.data() + *result);
        on_data_received(std::span<const std::byte>(temp.data(), *result));

        // Backpressure: if unread recv data too large, condemn
        if (recv_buffer_.size() - recv_read_pos_ > kMaxRecvBufferSize)
        {
            ATLAS_LOG_ERROR("TcpChannel recv buffer overflow from {}", remote_.to_string());
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
        std::size_t available = recv_buffer_.size() - recv_read_pos_;

        if (available < kFrameHeaderSize)
        {
            break;
        }

        const std::byte* head = recv_buffer_.data() + recv_read_pos_;

        uint32_t frame_length;
        std::memcpy(&frame_length, head, sizeof(uint32_t));
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

        const std::byte* frame_start = head + kFrameHeaderSize;
        dispatch_messages(std::span<const std::byte>(frame_start, frame_length));

        recv_read_pos_ += total;

        // Compact: once the consumed prefix exceeds half the buffer, shift.
        if (recv_read_pos_ > recv_buffer_.size() / 2)
        {
            recv_buffer_.erase(recv_buffer_.begin(),
                               recv_buffer_.begin() + static_cast<std::ptrdiff_t>(recv_read_pos_));
            recv_read_pos_ = 0;
        }
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
    auto* header_bytes = reinterpret_cast<const std::byte*>(&frame_length);

    write_buffer_.insert(write_buffer_.end(), header_bytes, header_bytes + sizeof(uint32_t));
    write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());

    try_flush_write_buffer();

    return static_cast<size_t>(data.size());
}

void TcpChannel::try_flush_write_buffer()
{
    while (write_read_pos_ < write_buffer_.size())
    {
        std::span<const std::byte> unsent(write_buffer_.data() + write_read_pos_,
                                          write_buffer_.size() - write_read_pos_);

        auto result = socket_.send(unsent);
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

        write_read_pos_ += *result;

        // Compact once consumed prefix exceeds half the buffer.
        if (write_read_pos_ > write_buffer_.size() / 2)
        {
            write_buffer_.erase(
                write_buffer_.begin(),
                write_buffer_.begin() + static_cast<std::ptrdiff_t>(write_read_pos_));
            write_read_pos_ = 0;
        }
    }

    // All data flushed — reset to empty state without reallocation.
    write_buffer_.clear();
    write_read_pos_ = 0;

    if (write_registered_)
    {
        update_write_interest();
    }
}

void TcpChannel::update_write_interest()
{
    bool need_write = (write_read_pos_ < write_buffer_.size());
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
