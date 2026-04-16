#ifndef ATLAS_LIB_NETWORK_COMPRESSION_FILTER_H_
#define ATLAS_LIB_NETWORK_COMPRESSION_FILTER_H_

#include <cstdint>

#include "network/packet_filter.h"

namespace atlas {

/// Compression type for network messages.
enum class CompressionType : uint8_t {
  kNone = 0,
  /// zlib deflate compression — low-latency level 1 (Z_BEST_SPEED).
  kDeflate = 1,
};

/// Packet filter that compresses data above a size threshold.
/// Wire format: [uint8 compression_type] [uint32 original_len (LE, only if compressed)] [data]
/// Only compresses if the payload exceeds the threshold AND compression
/// actually reduces the size.
class CompressionFilter : public PacketFilter {
 public:
  /// @param threshold Minimum payload size (bytes) before compression is attempted.
  ///                  Small packets are passed through uncompressed.
  /// @param type      Compression algorithm to use.
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
