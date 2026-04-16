#include "server/ipv4_networks.h"

#include <format>
#include <limits>
#include <string_view>

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace atlas {

namespace {

auto ParsePrefixLength(std::string_view text) -> Result<uint8_t> {
  if (text.empty()) {
    return Error{ErrorCode::kInvalidArgument, "missing CIDR prefix length"};
  }

  try {
    const int kValue = std::stoi(std::string(text));
    if (kValue < 0 || kValue > 32) {
      return Error{ErrorCode::kInvalidArgument,
                   std::format("CIDR prefix length out of range: {}", kValue)};
    }
    return static_cast<uint8_t>(kValue);
  } catch (...) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("invalid CIDR prefix length: '{}'", text)};
  }
}

auto ParseIpv4Literal(std::string_view text) -> Result<uint32_t> {
  uint32_t octets[4]{};
  std::size_t octet_index = 0;
  std::size_t cursor = 0;

  while (cursor < text.size() && octet_index < 4) {
    const auto kNextDot = text.find('.', cursor);
    const auto kToken = text.substr(
        cursor, kNextDot == std::string_view::npos ? text.size() - cursor : kNextDot - cursor);
    if (kToken.empty()) {
      return Error{ErrorCode::kInvalidArgument, std::format("invalid IPv4 address '{}'", text)};
    }

    try {
      const auto kValue = std::stoul(std::string(kToken));
      if (kValue > 255) {
        return Error{ErrorCode::kInvalidArgument,
                     std::format("IPv4 octet out of range in '{}'", text)};
      }
      octets[octet_index++] = static_cast<uint32_t>(kValue);
    } catch (...) {
      return Error{ErrorCode::kInvalidArgument, std::format("invalid IPv4 address '{}'", text)};
    }

    if (kNextDot == std::string_view::npos) {
      cursor = text.size();
      break;
    }
    cursor = kNextDot + 1;
  }

  if (octet_index != 4 || cursor < text.size()) {
    return Error{ErrorCode::kInvalidArgument, std::format("invalid IPv4 address '{}'", text)};
  }

  return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

auto PrefixToMask(uint8_t prefix_length) -> uint32_t {
  if (prefix_length == 0) {
    return 0;
  }
  if (prefix_length >= 32) {
    return (std::numeric_limits<uint32_t>::max)();
  }
  return (std::numeric_limits<uint32_t>::max)() << (32 - prefix_length);
}

}  // namespace

auto IPv4Network::contains(uint32_t ip_network_order) const -> bool {
  const auto kIpHostOrder = ntohl(ip_network_order);
  return (kIpHostOrder & mask_host_order) == network_host_order;
}

auto IPv4NetworkSet::Add(std::string_view spec) -> Result<void> {
  if (spec.empty()) {
    return Error{ErrorCode::kInvalidArgument, "empty IPv4 network specification"};
  }

  const auto kSlash = spec.find('/');
  const auto kHostText = spec.substr(0, kSlash);
  auto ip_host_order = ParseIpv4Literal(kHostText);
  if (!ip_host_order) {
    return ip_host_order.Error();
  }

  uint8_t prefix_length = 32;
  if (kSlash != std::string_view::npos) {
    auto prefix = ParsePrefixLength(spec.substr(kSlash + 1));
    if (!prefix) {
      return prefix.Error();
    }
    prefix_length = *prefix;
  }

  const auto kMaskHostOrder = PrefixToMask(prefix_length);
  const auto kNetworkHostOrder = *ip_host_order & kMaskHostOrder;

  networks_.push_back(IPv4Network{
      .network_host_order = kNetworkHostOrder,
      .mask_host_order = kMaskHostOrder,
      .prefix_length = prefix_length,
      .spec = std::string(spec),
  });
  return {};
}

auto IPv4NetworkSet::AddAll(const std::vector<std::string>& specs) -> Result<void> {
  for (const auto& spec : specs) {
    auto result = Add(spec);
    if (!result) {
      return result.Error();
    }
  }
  return {};
}

auto IPv4NetworkSet::contains(uint32_t ip_network_order) const -> bool {
  for (const auto& network : networks_) {
    if (network.contains(ip_network_order)) {
      return true;
    }
  }
  return false;
}

}  // namespace atlas
