#ifndef ATLAS_LIB_NETWORK_UDP_CHANNEL_H_
#define ATLAS_LIB_NETWORK_UDP_CHANNEL_H_

#include "network/channel.h"
#include "network/socket.h"

namespace atlas {

// Each datagram is a complete bundle; socket is owned by NetworkInterface.
class UdpChannel : public Channel {
 public:
  UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
             const Address& remote);
  ~UdpChannel() override = default;

  [[nodiscard]] auto Fd() const -> FdHandle override { return shared_socket_.Fd(); }

  void OnDatagramReceived(std::span<const std::byte> data);

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;

 private:
  Socket& shared_socket_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_UDP_CHANNEL_H_
