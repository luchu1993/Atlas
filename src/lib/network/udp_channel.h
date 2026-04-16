#ifndef ATLAS_LIB_NETWORK_UDP_CHANNEL_H_
#define ATLAS_LIB_NETWORK_UDP_CHANNEL_H_

#include "network/channel.h"
#include "network/socket.h"

namespace atlas {

// UDP channel — shares a socket with NetworkInterface.
// Each datagram is a complete bundle (no framing needed).
class UdpChannel : public Channel {
 public:
  // Does NOT own the socket — NetworkInterface owns the UDP socket.
  UdpChannel(EventDispatcher& dispatcher, InterfaceTable& table, Socket& shared_socket,
             const Address& remote);
  ~UdpChannel() override = default;

  [[nodiscard]] auto Fd() const -> FdHandle override { return shared_socket_.Fd(); }

  // Called by NetworkInterface when a datagram arrives from this channel's remote
  void OnDatagramReceived(std::span<const std::byte> data);

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override;

 private:
  Socket& shared_socket_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_UDP_CHANNEL_H_
