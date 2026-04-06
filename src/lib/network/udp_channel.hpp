#pragma once

#include "network/channel.hpp"
#include "network/socket.hpp"

namespace atlas
{

// UDP channel — shares a socket with NetworkInterface.
// Each datagram is a complete bundle (no framing needed).
class UdpChannel : public Channel
{
public:
    // Does NOT own the socket — NetworkInterface owns the UDP socket.
    UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
               Socket& shared_socket, const Address& remote);
    ~UdpChannel() override = default;

    [[nodiscard]] auto fd() const -> FdHandle override { return shared_socket_.fd(); }

    // Called by NetworkInterface when a datagram arrives from this channel's remote
    void on_datagram_received(std::span<const std::byte> data);

protected:
    auto do_send(std::span<const std::byte> data) -> Result<size_t> override;

private:
    Socket& shared_socket_;
};

} // namespace atlas
