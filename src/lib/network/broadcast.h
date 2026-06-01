#ifndef ATLAS_LIB_NETWORK_BROADCAST_H_
#define ATLAS_LIB_NETWORK_BROADCAST_H_

#include <cstdint>

#include "network/address.h"

namespace atlas {

// 255.255.255.255: the limited broadcast that reaches only the local segment.
inline constexpr uint32_t kLimitedBroadcastIp = 0xFFFFFFFFu;

[[nodiscard]] constexpr auto LimitedBroadcastAddress(uint16_t port) -> Address {
  return Address(kLimitedBroadcastIp, port);
}

// host_ip and netmask are network byte order (as Address::Ip stores them); the
// bitwise result is order-agnostic, so the returned IP is network order too.
[[nodiscard]] constexpr auto DirectedBroadcastIp(uint32_t host_ip_net, uint32_t netmask_net)
    -> uint32_t {
  return host_ip_net | ~netmask_net;
}

[[nodiscard]] constexpr auto DirectedBroadcastAddress(uint32_t host_ip_net, uint32_t netmask_net,
                                                      uint16_t port) -> Address {
  return Address(DirectedBroadcastIp(host_ip_net, netmask_net), port);
}

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BROADCAST_H_
