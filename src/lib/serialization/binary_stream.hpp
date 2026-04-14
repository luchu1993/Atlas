#pragma once

#include "foundation/error.hpp"
#include "platform/platform_config.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace atlas
{

// ============================================================================
// Trivial concept
// ============================================================================

template <typename T>
concept Trivial = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

// ============================================================================
// Endianness helpers
// ============================================================================

namespace endian
{

[[nodiscard]] constexpr auto byte_swap(uint16_t v) -> uint16_t
{
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

[[nodiscard]] constexpr auto byte_swap(uint32_t v) -> uint32_t
{
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x000000FFu) << 24);
}

[[nodiscard]] constexpr auto byte_swap(uint64_t v) -> uint64_t
{
    return ((v & 0xFF00000000000000ULL) >> 56) | ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0x0000FF0000000000ULL) >> 24) | ((v & 0x000000FF00000000ULL) >> 8) |
           ((v & 0x00000000FF000000ULL) << 8) | ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x000000000000FF00ULL) << 40) | ((v & 0x00000000000000FFULL) << 56);
}

template <Trivial T>
[[nodiscard]] constexpr auto to_little(T value) -> T
{
    if constexpr (std::endian::native == std::endian::little || sizeof(T) == 1)
    {
        return value;
    }
    else if constexpr (sizeof(T) == 2)
    {
        uint16_t v;
        std::memcpy(&v, &value, 2);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 2);
        return result;
    }
    else if constexpr (sizeof(T) == 4)
    {
        uint32_t v;
        std::memcpy(&v, &value, 4);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 4);
        return result;
    }
    else if constexpr (sizeof(T) == 8)
    {
        uint64_t v;
        std::memcpy(&v, &value, 8);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 8);
        return result;
    }
    else
    {
        static_assert(sizeof(T) == 1, "Unsupported type size for endian conversion");
        return value;
    }
}

template <Trivial T>
[[nodiscard]] constexpr auto from_little(T value) -> T
{
    return to_little(value);
}

template <Trivial T>
[[nodiscard]] constexpr auto to_big(T value) -> T
{
    if constexpr (std::endian::native == std::endian::big || sizeof(T) == 1)
    {
        return value;
    }
    else if constexpr (sizeof(T) == 2)
    {
        uint16_t v;
        std::memcpy(&v, &value, 2);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 2);
        return result;
    }
    else if constexpr (sizeof(T) == 4)
    {
        uint32_t v;
        std::memcpy(&v, &value, 4);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 4);
        return result;
    }
    else if constexpr (sizeof(T) == 8)
    {
        uint64_t v;
        std::memcpy(&v, &value, 8);
        v = byte_swap(v);
        T result;
        std::memcpy(&result, &v, 8);
        return result;
    }
    else
    {
        static_assert(sizeof(T) == 1, "Unsupported type size for endian conversion");
        return value;
    }
}

template <Trivial T>
[[nodiscard]] constexpr auto from_big(T value) -> T
{
    return to_big(value);
}

}  // namespace endian

// ============================================================================
// BinaryWriter
// ============================================================================

class BinaryWriter
{
public:
    BinaryWriter() = default;
    explicit BinaryWriter(std::size_t reserve_bytes);

    template <Trivial T>
    void write(T value)
    {
        auto le = endian::to_little(value);
        auto old_size = buffer_.size();
        buffer_.resize(old_size + sizeof(T));
        std::memcpy(buffer_.data() + old_size, &le, sizeof(T));
    }

    void write_bytes(std::span<const std::byte> data);
    void write_bytes(const void* data, std::size_t size);
    void write_string(std::string_view str);
    void write_packed_int(uint32_t value);

    [[nodiscard]] auto data() const -> std::span<const std::byte>;
    [[nodiscard]] auto mutable_data() -> std::byte* { return buffer_.data(); }
    [[nodiscard]] auto size() const -> std::size_t;
    [[nodiscard]] auto reserve(std::size_t bytes) -> std::byte*;
    void truncate(std::size_t new_size);

    // Attach/detach the internal buffer for zero-copy bundle composition
    void attach(std::vector<std::byte> buf);
    void clear();
    [[nodiscard]] auto detach() -> std::vector<std::byte>;

private:
    std::vector<std::byte> buffer_;
};

// ============================================================================
// BinaryReader
// ============================================================================

class BinaryReader
{
public:
    BinaryReader() = default;
    explicit BinaryReader(std::span<const std::byte> data);

    template <Trivial T>
    [[nodiscard]] auto read() -> Result<T>
    {
        if (pos_ + sizeof(T) > data_.size())
        {
            error_ = true;
            return Error{ErrorCode::OutOfRange, "Read past end of stream"};
        }
        T value;
        std::memcpy(&value, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return endian::from_little(value);
    }

    [[nodiscard]] auto read_bytes(std::size_t count) -> Result<std::span<const std::byte>>;
    [[nodiscard]] auto read_string() -> Result<std::string>;
    [[nodiscard]] auto read_string_view() -> Result<std::string_view>;
    [[nodiscard]] auto read_packed_int() -> Result<uint32_t>;

    [[nodiscard]] auto data() const -> std::span<const std::byte> { return data_; }
    [[nodiscard]] auto remaining() const -> std::size_t;
    [[nodiscard]] auto position() const -> std::size_t;
    [[nodiscard]] auto peek() const -> Result<std::byte>;

    void skip(std::size_t bytes);
    void reset();

    [[nodiscard]] auto has_error() const -> bool { return error_; }

private:
    std::span<const std::byte> data_;
    std::size_t pos_{0};
    bool error_{false};
};

}  // namespace atlas
