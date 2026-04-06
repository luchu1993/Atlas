#pragma once

#include "foundation/error.hpp"

#include <cstdint>
#include <compare>
#include <functional>
#include <string>
#include <string_view>

struct sockaddr_in;

namespace atlas
{

class Address
{
public:
    constexpr Address() = default;
    Address(std::string_view ip, uint16_t port);
    constexpr Address(uint32_t ip_network_order, uint16_t port_host_order)
        : ip_(ip_network_order), port_(port_host_order) {}
    explicit Address(const sockaddr_in& sa);

    [[nodiscard]] constexpr auto ip() const -> uint32_t { return ip_; }
    [[nodiscard]] constexpr auto port() const -> uint16_t { return port_; }

    [[nodiscard]] auto to_string() const -> std::string;
    [[nodiscard]] auto to_sockaddr() const -> sockaddr_in;

    [[nodiscard]] static auto resolve(std::string_view hostname, uint16_t port)
        -> Result<Address>;

    constexpr auto operator==(const Address& other) const -> bool = default;
    constexpr auto operator<=>(const Address& other) const = default;

    static const Address NONE;

private:
    uint32_t ip_{0};      // network byte order
    uint16_t port_{0};    // host byte order
};

} // namespace atlas

template <>
struct std::hash<atlas::Address>
{
    auto operator()(const atlas::Address& addr) const noexcept -> std::size_t
    {
        auto h1 = std::hash<uint32_t>{}(addr.ip());
        auto h2 = std::hash<uint16_t>{}(addr.port());
        return h1 ^ (h2 << 16);
    }
};
