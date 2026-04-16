#include "network/udp_channel.h"

#include "foundation/log.h"

namespace atlas {

UdpChannel::UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
                       const Address& remote)
    : Channel(dispatcher, table, remote), shared_socket_(shared_socket) {}

void UdpChannel::OnDatagramReceived(std::span<const std::byte> data) {
  OnDataReceived(data);
  // Each datagram is a complete bundle — dispatch messages directly
  DispatchMessages(data);
}

auto UdpChannel::DoSend(std::span<const std::byte> data) -> Result<size_t> {
  // UDP: no framing needed, each send is a complete datagram
  auto result = shared_socket_.SendTo(data, remote_);
  if (!result) {
    return result.Error();
  }
  return *result;
}

}  // namespace atlas
