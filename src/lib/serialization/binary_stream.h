#ifndef ATLAS_LIB_SERIALIZATION_BINARY_STREAM_H_
#define ATLAS_LIB_SERIALIZATION_BINARY_STREAM_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "foundation/error.h"
#include "platform/platform_config.h"

namespace atlas {

template <typename T>
concept Trivial = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

namespace endian {

[[nodiscard]] constexpr auto ByteSwap(uint16_t v) -> uint16_t {
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

[[nodiscard]] constexpr auto ByteSwap(uint32_t v) -> uint32_t {
  return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x000000FFu) << 24);
}

[[nodiscard]] constexpr auto ByteSwap(uint64_t v) -> uint64_t {
  return ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40) |
         ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >> 8) |
         ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x0000000000FF0000ULL) << 24) |
         ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
}

template <Trivial T>
[[nodiscard]] constexpr auto ToLittle(T value) -> T {
  if constexpr (std::endian::native == std::endian::little || sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    uint16_t v;
    std::memcpy(&v, &value, 2);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 2);
    return result;
  } else if constexpr (sizeof(T) == 4) {
    uint32_t v;
    std::memcpy(&v, &value, 4);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 4);
    return result;
  } else if constexpr (sizeof(T) == 8) {
    uint64_t v;
    std::memcpy(&v, &value, 8);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 8);
    return result;
  } else {
    static_assert(sizeof(T) == 1, "Unsupported type size for endian conversion");
    return value;
  }
}

template <Trivial T>
[[nodiscard]] constexpr auto FromLittle(T value) -> T {
  return ToLittle(value);
}

template <Trivial T>
[[nodiscard]] constexpr auto ToBig(T value) -> T {
  if constexpr (std::endian::native == std::endian::big || sizeof(T) == 1) {
    return value;
  } else if constexpr (sizeof(T) == 2) {
    uint16_t v;
    std::memcpy(&v, &value, 2);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 2);
    return result;
  } else if constexpr (sizeof(T) == 4) {
    uint32_t v;
    std::memcpy(&v, &value, 4);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 4);
    return result;
  } else if constexpr (sizeof(T) == 8) {
    uint64_t v;
    std::memcpy(&v, &value, 8);
    v = ByteSwap(v);
    T result;
    std::memcpy(&result, &v, 8);
    return result;
  } else {
    static_assert(sizeof(T) == 1, "Unsupported type size for endian conversion");
    return value;
  }
}

template <Trivial T>
[[nodiscard]] constexpr auto FromBig(T value) -> T {
  return ToBig(value);
}

}  // namespace endian

class BinaryWriter {
 public:
  BinaryWriter() = default;
  explicit BinaryWriter(std::size_t reserve_bytes);

  template <Trivial T>
  void Write(T value) {
    auto le = endian::ToLittle(value);
    auto old_size = buffer_.size();
    buffer_.resize(old_size + sizeof(T));
    std::memcpy(buffer_.data() + old_size, &le, sizeof(T));
  }

  void WriteBytes(std::span<const std::byte> data);
  void WriteBytes(const void* data, std::size_t size);
  void WriteString(std::string_view str);
  void WritePackedInt(uint32_t value);

  [[nodiscard]] auto Data() const -> std::span<const std::byte>;
  [[nodiscard]] auto MutableData() -> std::byte* { return buffer_.data(); }
  [[nodiscard]] auto Size() const -> std::size_t;
  [[nodiscard]] auto Reserve(std::size_t bytes) -> std::byte*;
  void Truncate(std::size_t new_size);

  void Attach(std::vector<std::byte> buf);
  void Clear();
  [[nodiscard]] auto Detach() -> std::vector<std::byte>;

 private:
  std::vector<std::byte> buffer_;
};

class BinaryReader {
 public:
  BinaryReader() = default;
  explicit BinaryReader(std::span<const std::byte> data);

  template <Trivial T>
  [[nodiscard]] auto Read() -> Result<T> {
    if (pos_ + sizeof(T) > data_.size()) {
      error_ = true;
      return Error{ErrorCode::kOutOfRange, "Read past end of stream"};
    }
    T value;
    std::memcpy(&value, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return endian::FromLittle(value);
  }

  [[nodiscard]] auto ReadBytes(std::size_t count) -> Result<std::span<const std::byte>>;
  [[nodiscard]] auto ReadString() -> Result<std::string>;
  [[nodiscard]] auto ReadStringView() -> Result<std::string_view>;
  [[nodiscard]] auto ReadPackedInt() -> Result<uint32_t>;

  [[nodiscard]] auto Data() const -> std::span<const std::byte> { return data_; }
  [[nodiscard]] auto Remaining() const -> std::size_t;
  [[nodiscard]] auto Position() const -> std::size_t;
  [[nodiscard]] auto Peek() const -> Result<std::byte>;

  void Skip(std::size_t bytes);
  void Reset();

  [[nodiscard]] auto HasError() const -> bool { return error_; }

 private:
  std::span<const std::byte> data_;
  std::size_t pos_{0};
  bool error_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERIALIZATION_BINARY_STREAM_H_
