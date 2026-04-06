#pragma once

#include "network/channel.hpp"
#include "network/socket.hpp"

#include <vector>

namespace atlas
{

class TcpChannel : public Channel
{
public:
    // TCP frame format: [uint32 frame_length LE][bundle data]
    static constexpr std::size_t kFrameHeaderSize = sizeof(uint32_t);
    static constexpr std::size_t kMaxRecvBufferSize = 1024 * 1024;  // 1 MB backpressure limit

    TcpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
               Socket socket, const Address& remote);
    ~TcpChannel() override;

    [[nodiscard]] auto fd() const -> FdHandle override { return socket_.fd(); }

    // Called when socket is readable (from EventDispatcher)
    void on_readable();

    // Called when socket is writable (flush write buffer)
    void on_writable();

protected:
    [[nodiscard]] auto do_send(std::span<const std::byte> data) -> Result<size_t> override;

private:
    void process_recv_buffer();
    void try_flush_write_buffer();
    void update_write_interest();

    Socket socket_;
    std::vector<std::byte> recv_buffer_;
    std::vector<std::byte> write_buffer_;
    bool write_registered_{false};
};

} // namespace atlas
