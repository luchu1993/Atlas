#ifndef ATLAS_LIB_NETWORK_PACKET_FILTER_H_
#define ATLAS_LIB_NETWORK_PACKET_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "foundation/error.h"

namespace atlas {

/// Abstract interface for packet-level filtering (compression, encryption, etc.).
/// Filters are applied at the Channel level, transforming data between Bundle
/// serialization and transport send/recv.
class PacketFilter {
 public:
  virtual ~PacketFilter() = default;

  /// Transform outgoing data before it is sent on the wire.
  /// Returns the filtered data. Default implementation passes through unchanged.
  [[nodiscard]] virtual auto SendFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> {
    return std::vector<std::byte>(data.begin(), data.end());
  }

  /// Transform incoming data after it is received from the wire.
  /// Returns the filtered data. Default implementation passes through unchanged.
  [[nodiscard]] virtual auto RecvFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> {
    return std::vector<std::byte>(data.begin(), data.end());
  }

  /// Maximum extra bytes the filter may add to a packet (for buffer reservation).
  [[nodiscard]] virtual auto MaxOverhead() const -> std::size_t { return 0; }
};

using PacketFilterPtr = std::shared_ptr<PacketFilter>;

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_PACKET_FILTER_H_
