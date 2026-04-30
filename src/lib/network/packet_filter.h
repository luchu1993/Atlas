#ifndef ATLAS_LIB_NETWORK_PACKET_FILTER_H_
#define ATLAS_LIB_NETWORK_PACKET_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "foundation/error.h"

namespace atlas {

// Channel-level transform between bundle serialization and transport I/O.
class PacketFilter {
 public:
  virtual ~PacketFilter() = default;

  [[nodiscard]] virtual auto SendFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> {
    return std::vector<std::byte>(data.begin(), data.end());
  }

  [[nodiscard]] virtual auto RecvFilter(std::span<const std::byte> data)
      -> Result<std::vector<std::byte>> {
    return std::vector<std::byte>(data.begin(), data.end());
  }

  [[nodiscard]] virtual auto MaxOverhead() const -> std::size_t { return 0; }
};

using PacketFilterPtr = std::shared_ptr<PacketFilter>;

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_PACKET_FILTER_H_
