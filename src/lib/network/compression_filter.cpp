#include "network/compression_filter.hpp"

#include "foundation/log.hpp"
#include "serialization/binary_stream.hpp"

#include <cstring>
#include <zlib.h>

namespace atlas
{

CompressionFilter::CompressionFilter(std::size_t threshold, CompressionType type)
    : threshold_(threshold), type_(type)
{
}

auto CompressionFilter::send_filter(std::span<const std::byte> data)
    -> Result<std::vector<std::byte>>
{
    if (data.size() < threshold_ || type_ == CompressionType::None)
    {
        std::vector<std::byte> result(1 + data.size());
        result[0] = static_cast<std::byte>(CompressionType::None);
        std::memcpy(result.data() + 1, data.data(), data.size());
        return result;
    }

    // Compress with zlib deflate at Z_BEST_SPEED for low latency
    uLongf compressed_bound = compressBound(static_cast<uLong>(data.size()));

    // Header: [uint8 type=1][uint32 original_len LE] = 5 bytes
    constexpr std::size_t kHeaderSize = 5;
    std::vector<std::byte> output(kHeaderSize + compressed_bound);

    output[0] = static_cast<std::byte>(CompressionType::Deflate);
    auto original_len = endian::to_little(static_cast<uint32_t>(data.size()));
    std::memcpy(output.data() + 1, &original_len, sizeof(uint32_t));

    uLongf dest_len = compressed_bound;
    int ret = compress2(reinterpret_cast<Bytef*>(output.data() + kHeaderSize), &dest_len,
                        reinterpret_cast<const Bytef*>(data.data()),
                        static_cast<uLong>(data.size()), Z_BEST_SPEED);

    if (ret != Z_OK)
    {
        return Error(ErrorCode::InternalError, "zlib compress2 failed");
    }

    std::size_t total_compressed = kHeaderSize + dest_len;

    // Fall back to uncompressed if compression didn't help
    if (total_compressed >= 1 + data.size())
    {
        std::vector<std::byte> result(1 + data.size());
        result[0] = static_cast<std::byte>(CompressionType::None);
        std::memcpy(result.data() + 1, data.data(), data.size());
        return result;
    }

    output.resize(total_compressed);
    return output;
}

auto CompressionFilter::recv_filter(std::span<const std::byte> data)
    -> Result<std::vector<std::byte>>
{
    if (data.size() < 1)
    {
        return Error(ErrorCode::InvalidArgument, "CompressionFilter: empty packet");
    }

    auto tag = static_cast<CompressionType>(data[0]);

    if (tag == CompressionType::None)
    {
        return std::vector<std::byte>(data.begin() + 1, data.end());
    }

    if (tag == CompressionType::Deflate)
    {
        constexpr std::size_t kHeaderSize = 5;
        if (data.size() < kHeaderSize)
        {
            return Error(ErrorCode::InvalidArgument, "CompressionFilter: truncated deflate header");
        }

        uint32_t original_len_le;
        std::memcpy(&original_len_le, data.data() + 1, sizeof(uint32_t));
        auto original_len = endian::from_little(original_len_le);

        if (original_len > 16 * 1024 * 1024)
        {
            return Error(ErrorCode::InvalidArgument,
                         "CompressionFilter: decompressed size exceeds 16 MB limit");
        }

        std::vector<std::byte> output(original_len);
        uLongf dest_len = original_len;

        int ret = uncompress(reinterpret_cast<Bytef*>(output.data()), &dest_len,
                             reinterpret_cast<const Bytef*>(data.data() + kHeaderSize),
                             static_cast<uLong>(data.size() - kHeaderSize));

        if (ret != Z_OK)
        {
            return Error(ErrorCode::InternalError, "zlib uncompress failed");
        }

        output.resize(dest_len);
        return output;
    }

    return Error(ErrorCode::InvalidArgument, "CompressionFilter: unknown compression type");
}

auto CompressionFilter::max_overhead() const -> std::size_t
{
    // 1 byte type tag + 4 bytes original length
    return 5;
}

}  // namespace atlas
