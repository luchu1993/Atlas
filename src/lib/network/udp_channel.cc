#include "network/udp_channel.h"

#include "foundation/log.h"

namespace atlas {

UdpChannel::UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
                       const Address& remote)
    : Channel(dispatcher, table, remote), shared_socket_(shared_socket) {}

void UdpChannel::OnDatagramReceived(std::span<const std::byte> data) {
  OnDataReceived(data);
  DispatchMessages(data);
}

auto UdpChannel::DoSend(std::span<const std::byte> data) -> Result<size_t> {
  auto result = shared_socket_.SendTo(data, remote_);
  if (!result) {
    return result.Error();
  }
  return *result;
}

}  // namespace atlas
