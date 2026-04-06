#include "network/udp_channel.hpp"

#include "foundation/log.hpp"

namespace atlas
{

UdpChannel::UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table,
                       Socket& shared_socket, const Address& remote)
    : Channel(dispatcher, table, remote)
    , shared_socket_(shared_socket)
{
}

void UdpChannel::on_datagram_received(std::span<const std::byte> data)
{
    on_data_received(data);
    // Each datagram is a complete bundle — dispatch messages directly
    dispatch_messages(data);
}

auto UdpChannel::do_send(std::span<const std::byte> data) -> Result<size_t>
{
    // UDP: no framing needed, each send is a complete datagram
    auto result = shared_socket_.send_to(data, remote_);
    if (!result)
    {
        return result.error();
    }
    return *result;
}

} // namespace atlas
