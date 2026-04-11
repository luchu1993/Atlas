#pragma once

#include "foundation/containers/byte_ring_buffer.hpp"
#include "network/channel.hpp"
#include "network/socket.hpp"

namespace atlas
{

class TcpChannel : public Channel
{
public:
    static constexpr std::size_t kFrameHeaderSize = sizeof(uint32_t);
    static constexpr std::size_t kMaxRecvBufferSize = 1024 * 1024;

    TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket socket,
               const Address& remote);
    ~TcpChannel() override;

    [[nodiscard]] auto fd() const -> FdHandle override { return socket_.fd(); }

    void on_readable();
    void on_writable();

protected:
    [[nodiscard]] auto do_send(std::span<const std::byte> data) -> Result<size_t> override;

private:
    void process_recv_buffer();
    void try_flush_write_buffer();
    void update_write_interest();

    Socket socket_;

    ByteRingBuffer recv_buffer_{kMaxRecvBufferSize};
    ByteRingBuffer write_buffer_{256 * 1024};

    bool write_registered_{false};
};

}  // namespace atlas
