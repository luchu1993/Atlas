#include "network/compression_filter.h"

#include <cstring>

#include <zlib.h>

#include "foundation/log.h"
#include "serialization/binary_stream.h"

namespace atlas {

namespace {

// memcpy(dst, nullptr, 0) is UB even when n == 0, and span::data() may
// return null for an empty range — guard the source pointer here.
auto WrapUncompressed(std::span<const std::byte> data) -> std::vector<std::byte> {
  std::vector<std::byte> result(1 + data.size());
  result[0] = static_cast<std::byte>(CompressionType::kNone);
  if (!data.empty()) {
    std::memcpy(result.data() + 1, data.data(), data.size());
  }
  return result;
}

}  // namespace

CompressionFilter::CompressionFilter(std::size_t threshold, CompressionType type)
    : threshold_(threshold), type_(type) {}

auto CompressionFilter::SendFilter(std::span<const std::byte> data)
    -> Result<std::vector<std::byte>> {
  if (data.size() < threshold_ || type_ == CompressionType::kNone) {
    return WrapUncompressed(data);
  }

  uLongf compressed_bound = compressBound(static_cast<uLong>(data.size()));

  constexpr std::size_t kHeaderSize = 5;
  std::vector<std::byte> output(kHeaderSize + compressed_bound);

  output[0] = static_cast<std::byte>(CompressionType::kDeflate);
  auto original_len = endian::ToLittle(static_cast<uint32_t>(data.size()));
  std::memcpy(output.data() + 1, &original_len, sizeof(uint32_t));

  uLongf dest_len = compressed_bound;
  int ret = compress2(reinterpret_cast<Bytef*>(output.data() + kHeaderSize), &dest_len,
                      reinterpret_cast<const Bytef*>(data.data()), static_cast<uLong>(data.size()),
                      Z_BEST_SPEED);

  if (ret != Z_OK) {
    return Error(ErrorCode::kInternalError, "zlib compress2 failed");
  }

  std::size_t total_compressed = kHeaderSize + dest_len;

  if (total_compressed >= 1 + data.size()) {
    return WrapUncompressed(data);
  }

  output.resize(total_compressed);
  return output;
}

auto CompressionFilter::RecvFilter(std::span<const std::byte> data)
    -> Result<std::vector<std::byte>> {
  if (data.size() < 1) {
    return Error(ErrorCode::kInvalidArgument, "CompressionFilter: empty packet");
  }

  auto tag = static_cast<CompressionType>(data[0]);

  if (tag == CompressionType::kNone) {
    return std::vector<std::byte>(data.begin() + 1, data.end());
  }

  if (tag == CompressionType::kDeflate) {
    constexpr std::size_t kHeaderSize = 5;
    if (data.size() < kHeaderSize) {
      return Error(ErrorCode::kInvalidArgument, "CompressionFilter: truncated deflate header");
    }

    uint32_t original_len_le;
    std::memcpy(&original_len_le, data.data() + 1, sizeof(uint32_t));
    auto original_len = endian::FromLittle(original_len_le);

    if (original_len > 16 * 1024 * 1024) {
      return Error(ErrorCode::kInvalidArgument,
                   "CompressionFilter: decompressed size exceeds 16 MB limit");
    }

    std::vector<std::byte> output(original_len);
    uLongf dest_len = original_len;

    int ret = uncompress(reinterpret_cast<Bytef*>(output.data()), &dest_len,
                         reinterpret_cast<const Bytef*>(data.data() + kHeaderSize),
                         static_cast<uLong>(data.size() - kHeaderSize));

    if (ret != Z_OK) {
      return Error(ErrorCode::kInternalError, "zlib uncompress failed");
    }

    output.resize(dest_len);
    return output;
  }

  return Error(ErrorCode::kInvalidArgument, "CompressionFilter: unknown compression type");
}

auto CompressionFilter::MaxOverhead() const -> std::size_t {
  return 5;
}

}  // namespace atlas
