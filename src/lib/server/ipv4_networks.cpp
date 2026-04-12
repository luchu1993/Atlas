#include "server/ipv4_networks.hpp"

#include <format>
#include <limits>
#include <string_view>

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace atlas
{

namespace
{

auto parse_prefix_length(std::string_view text) -> Result<uint8_t>
{
    if (text.empty())
    {
        return Error{ErrorCode::InvalidArgument, "missing CIDR prefix length"};
    }

    try
    {
        const int value = std::stoi(std::string(text));
        if (value < 0 || value > 32)
        {
            return Error{ErrorCode::InvalidArgument,
                         std::format("CIDR prefix length out of range: {}", value)};
        }
        return static_cast<uint8_t>(value);
    }
    catch (...)
    {
        return Error{ErrorCode::InvalidArgument,
                     std::format("invalid CIDR prefix length: '{}'", text)};
    }
}

auto parse_ipv4_literal(std::string_view text) -> Result<uint32_t>
{
    uint32_t octets[4]{};
    std::size_t octet_index = 0;
    std::size_t cursor = 0;

    while (cursor < text.size() && octet_index < 4)
    {
        const auto next_dot = text.find('.', cursor);
        const auto token = text.substr(
            cursor, next_dot == std::string_view::npos ? text.size() - cursor : next_dot - cursor);
        if (token.empty())
        {
            return Error{ErrorCode::InvalidArgument,
                         std::format("invalid IPv4 address '{}'", text)};
        }

        try
        {
            const auto value = std::stoul(std::string(token));
            if (value > 255)
            {
                return Error{ErrorCode::InvalidArgument,
                             std::format("IPv4 octet out of range in '{}'", text)};
            }
            octets[octet_index++] = static_cast<uint32_t>(value);
        }
        catch (...)
        {
            return Error{ErrorCode::InvalidArgument,
                         std::format("invalid IPv4 address '{}'", text)};
        }

        if (next_dot == std::string_view::npos)
        {
            cursor = text.size();
            break;
        }
        cursor = next_dot + 1;
    }

    if (octet_index != 4 || cursor < text.size())
    {
        return Error{ErrorCode::InvalidArgument, std::format("invalid IPv4 address '{}'", text)};
    }

    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

auto prefix_to_mask(uint8_t prefix_length) -> uint32_t
{
    if (prefix_length == 0)
    {
        return 0;
    }
    if (prefix_length >= 32)
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return std::numeric_limits<uint32_t>::max() << (32 - prefix_length);
}

}  // namespace

auto IPv4Network::contains(uint32_t ip_network_order) const -> bool
{
    const auto ip_host_order = ntohl(ip_network_order);
    return (ip_host_order & mask_host_order) == network_host_order;
}

auto IPv4NetworkSet::add(std::string_view spec) -> Result<void>
{
    if (spec.empty())
    {
        return Error{ErrorCode::InvalidArgument, "empty IPv4 network specification"};
    }

    const auto slash = spec.find('/');
    const auto host_text = spec.substr(0, slash);
    auto ip_host_order = parse_ipv4_literal(host_text);
    if (!ip_host_order)
    {
        return ip_host_order.error();
    }

    uint8_t prefix_length = 32;
    if (slash != std::string_view::npos)
    {
        auto prefix = parse_prefix_length(spec.substr(slash + 1));
        if (!prefix)
        {
            return prefix.error();
        }
        prefix_length = *prefix;
    }

    const auto mask_host_order = prefix_to_mask(prefix_length);
    const auto network_host_order = *ip_host_order & mask_host_order;

    networks_.push_back(IPv4Network{
        .network_host_order = network_host_order,
        .mask_host_order = mask_host_order,
        .prefix_length = prefix_length,
        .spec = std::string(spec),
    });
    return {};
}

auto IPv4NetworkSet::add_all(const std::vector<std::string>& specs) -> Result<void>
{
    for (const auto& spec : specs)
    {
        auto result = add(spec);
        if (!result)
        {
            return result.error();
        }
    }
    return {};
}

auto IPv4NetworkSet::contains(uint32_t ip_network_order) const -> bool
{
    for (const auto& network : networks_)
    {
        if (network.contains(ip_network_order))
        {
            return true;
        }
    }
    return false;
}

}  // namespace atlas
