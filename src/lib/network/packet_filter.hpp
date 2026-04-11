#pragma once

#include "foundation/error.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace atlas
{

/// Abstract interface for packet-level filtering (compression, encryption, etc.).
/// Filters are applied at the Channel level, transforming data between Bundle
/// serialization and transport send/recv.
class PacketFilter
{
public:
    virtual ~PacketFilter() = default;

    /// Transform outgoing data before it is sent on the wire.
    /// Returns the filtered data. Default implementation passes through unchanged.
    [[nodiscard]] virtual auto send_filter(std::span<const std::byte> data)
        -> Result<std::vector<std::byte>>
    {
        return std::vector<std::byte>(data.begin(), data.end());
    }

    /// Transform incoming data after it is received from the wire.
    /// Returns the filtered data. Default implementation passes through unchanged.
    [[nodiscard]] virtual auto recv_filter(std::span<const std::byte> data)
        -> Result<std::vector<std::byte>>
    {
        return std::vector<std::byte>(data.begin(), data.end());
    }

    /// Maximum extra bytes the filter may add to a packet (for buffer reservation).
    [[nodiscard]] virtual auto max_overhead() const -> std::size_t { return 0; }
};

using PacketFilterPtr = std::shared_ptr<PacketFilter>;

}  // namespace atlas
