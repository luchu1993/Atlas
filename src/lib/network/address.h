#ifndef ATLAS_LIB_NETWORK_ADDRESS_H_
#define ATLAS_LIB_NETWORK_ADDRESS_H_

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "foundation/error.h"

struct sockaddr_in;

namespace atlas {

class Address {
 public:
  constexpr Address() = default;
  Address(std::string_view ip, uint16_t port);
  constexpr Address(uint32_t ip_network_order, uint16_t port_host_order)
      : ip_(ip_network_order), port_(port_host_order) {}
  explicit Address(const sockaddr_in& sa);

  [[nodiscard]] constexpr auto Ip() const -> uint32_t { return ip_; }
  [[nodiscard]] constexpr auto Port() const -> uint16_t { return port_; }

  [[nodiscard]] auto ToString() const -> std::string;
  [[nodiscard]] auto ToSockaddr() const -> sockaddr_in;

  [[nodiscard]] static auto Resolve(std::string_view hostname, uint16_t port) -> Result<Address>;

  constexpr auto operator==(const Address& other) const -> bool = default;
  constexpr auto operator<=>(const Address& other) const = default;

  static const Address kNone;

 private:
  uint32_t ip_{0};    // network byte order
  uint16_t port_{0};  // host byte order
};

}  // namespace atlas

template <>
struct std::hash<atlas::Address> {
  auto operator()(const atlas::Address& addr) const noexcept -> std::size_t {
    // 64-bit mixing to avoid zero upper bits on 64-bit platforms.
    auto h = static_cast<std::size_t>(addr.Ip());
    h *= std::size_t{0x9E3779B97F4A7C15ULL};
    h ^= static_cast<std::size_t>(addr.Port()) * std::size_t{0x517CC1B727220A95ULL};
    return h;
  }
};

#endif  // ATLAS_LIB_NETWORK_ADDRESS_H_
