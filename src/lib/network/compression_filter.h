#ifndef ATLAS_LIB_NETWORK_COMPRESSION_FILTER_H_
#define ATLAS_LIB_NETWORK_COMPRESSION_FILTER_H_

#include <cstdint>

#include "network/packet_filter.h"

namespace atlas {

enum class CompressionType : uint8_t {
  kNone = 0,
  kDeflate = 1,
};

// Wire format: [uint8 type] [uint32 original_len LE, when compressed] [data].
class CompressionFilter : public PacketFilter {
 public:
  explicit CompressionFilter(std::size_t threshold = 256,
                             CompressionType type = CompressionType::kDeflate);

  [[nodiscard]] auto SendFilter(std::span<const std::byte> data) -> Result<std::vector<std::byte>>;

  [[nodiscard]] auto RecvFilter(std::span<const std::byte> data) -> Result<std::vector<std::byte>>;

  [[nodiscard]] auto MaxOverhead() const -> std::size_t;

 private:
  std::size_t threshold_;
  CompressionType type_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_COMPRESSION_FILTER_H_
