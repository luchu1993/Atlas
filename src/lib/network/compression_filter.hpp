#pragma once

#include "network/packet_filter.hpp"

#include <cstdint>

namespace atlas
{

/// Compression type for network messages.
enum class CompressionType : uint8_t
{
    None = 0,
    /// zlib deflate compression — low-latency level 1 (Z_BEST_SPEED).
    Deflate = 1,
};

/// Packet filter that compresses data above a size threshold.
/// Wire format: [uint8 compression_type] [uint32 original_len (LE, only if compressed)] [data]
/// Only compresses if the payload exceeds the threshold AND compression
/// actually reduces the size.
class CompressionFilter : public PacketFilter
{
public:
    /// @param threshold Minimum payload size (bytes) before compression is attempted.
    ///                  Small packets are passed through uncompressed.
    /// @param type      Compression algorithm to use.
    explicit CompressionFilter(std::size_t threshold = 256,
                               CompressionType type = CompressionType::Deflate);

    [[nodiscard]] auto send_filter(std::span<const std::byte> data)
        -> Result<std::vector<std::byte>> override;

    [[nodiscard]] auto recv_filter(std::span<const std::byte> data)
        -> Result<std::vector<std::byte>> override;

    [[nodiscard]] auto max_overhead() const -> std::size_t override;

private:
    std::size_t threshold_;
    CompressionType type_;
};

}  // namespace atlas
