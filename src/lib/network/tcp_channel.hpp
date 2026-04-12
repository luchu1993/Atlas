#pragma once

#include "foundation/containers/byte_ring_buffer.hpp"
#include "network/channel.hpp"
#include "network/socket.hpp"

#include <optional>

namespace atlas
{

class TcpChannel : public Channel
{
public:
    static constexpr std::size_t kFrameHeaderSize = sizeof(uint32_t);
    static constexpr std::size_t kInitialRecvBufferSize = 16 * 1024;
    static constexpr std::size_t kMaxRecvBufferSize = 1024 * 1024;
    static constexpr std::size_t kInitialWriteBufferSize = 16 * 1024;
    static constexpr std::size_t kMaxWriteBufferSize = 256 * 1024;
    static constexpr Duration kBufferShrinkDelay = std::chrono::seconds(30);
    static constexpr std::size_t kMaxSocketReadsPerCallback = 64;
    static constexpr std::size_t kMaxFramesPerCallback = 128;
    static constexpr std::size_t kMaxWriteFlushesPerCallback = 64;

    TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket socket,
               const Address& remote);
    ~TcpChannel() override;

    [[nodiscard]] auto fd() const -> FdHandle override { return socket_.fd(); }
    [[nodiscard]] auto recv_buffer_capacity() const -> std::size_t
    {
        return recv_buffer_.capacity();
    }
    [[nodiscard]] auto recv_buffer_size() const -> std::size_t
    {
        return recv_buffer_.readable_size();
    }
    [[nodiscard]] auto write_buffer_capacity() const -> std::size_t
    {
        return write_buffer_.capacity();
    }
    [[nodiscard]] auto write_buffer_size() const -> std::size_t
    {
        return write_buffer_.readable_size();
    }

    void on_readable();
    void on_writable();

protected:
    [[nodiscard]] auto do_send(std::span<const std::byte> data) -> Result<size_t> override;
    void on_condemned() override;

private:
    [[nodiscard]] auto ensure_recv_writable() -> bool;
    [[nodiscard]] auto peek_frame_length() const -> std::optional<uint32_t>;
    void process_recv_buffer(std::size_t frame_budget = kMaxFramesPerCallback);
    void try_flush_write_buffer(std::size_t write_budget = kMaxWriteFlushesPerCallback);
    void update_write_interest();
    void schedule_recv_buffer_shrink();
    void cancel_recv_buffer_shrink();
    void on_recv_buffer_shrink(TimerHandle handle);
    void schedule_write_buffer_shrink();
    void cancel_write_buffer_shrink();
    void on_write_buffer_shrink(TimerHandle handle);

    Socket socket_;

    ByteRingBuffer recv_buffer_{kInitialRecvBufferSize, kMaxRecvBufferSize};
    ByteRingBuffer write_buffer_{kInitialWriteBufferSize, kMaxWriteBufferSize};
    TimerHandle recv_buffer_shrink_timer_;
    TimerHandle write_buffer_shrink_timer_;

    bool write_registered_{false};
};

}  // namespace atlas
