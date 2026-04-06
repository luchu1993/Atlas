#include "serialization/binary_stream.hpp"

namespace atlas
{

// ============================================================================
// BinaryWriter
// ============================================================================

BinaryWriter::BinaryWriter(std::size_t reserve_bytes)
{
    buffer_.reserve(reserve_bytes);
}

void BinaryWriter::write_bytes(std::span<const std::byte> data)
{
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void BinaryWriter::write_bytes(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        return;
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void BinaryWriter::write_string(std::string_view str)
{
    write_packed_int(static_cast<uint32_t>(str.size()));
    write_bytes(str.data(), str.size());
}

void BinaryWriter::write_packed_int(uint32_t value)
{
    if (value < 0xFF)
    {
        write(static_cast<uint8_t>(value));
    }
    else
    {
        write(static_cast<uint8_t>(0xFF));
        write(value);
    }
}

auto BinaryWriter::data() const -> std::span<const std::byte>
{
    return {buffer_.data(), buffer_.size()};
}

auto BinaryWriter::size() const -> std::size_t
{
    return buffer_.size();
}

auto BinaryWriter::reserve(std::size_t bytes) -> std::byte*
{
    auto old_size = buffer_.size();
    buffer_.resize(old_size + bytes);
    return buffer_.data() + old_size;
}

void BinaryWriter::clear()
{
    buffer_.clear();
}

auto BinaryWriter::detach() -> std::vector<std::byte>
{
    return std::move(buffer_);
}

// ============================================================================
// BinaryReader
// ============================================================================

BinaryReader::BinaryReader(std::span<const std::byte> data) : data_(data) {}

auto BinaryReader::read_bytes(std::size_t count) -> Result<std::span<const std::byte>>
{
    if (pos_ + count > data_.size())
    {
        error_ = true;
        return Error{ErrorCode::OutOfRange, "Read past end of stream"};
    }
    auto result = data_.subspan(pos_, count);
    pos_ += count;
    return result;
}

auto BinaryReader::read_string() -> Result<std::string>
{
    auto length_result = read_packed_int();
    if (!length_result)
    {
        return length_result.error();
    }

    auto length = length_result.value();
    auto bytes_result = read_bytes(length);
    if (!bytes_result)
    {
        return bytes_result.error();
    }

    auto bytes = bytes_result.value();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto BinaryReader::read_packed_int() -> Result<uint32_t>
{
    auto tag_result = read<uint8_t>();
    if (!tag_result)
    {
        return tag_result.error();
    }

    auto tag = tag_result.value();
    if (tag < 0xFF)
    {
        return static_cast<uint32_t>(tag);
    }

    return read<uint32_t>();
}

auto BinaryReader::remaining() const -> std::size_t
{
    return data_.size() - pos_;
}

auto BinaryReader::position() const -> std::size_t
{
    return pos_;
}

auto BinaryReader::peek() const -> Result<std::byte>
{
    if (pos_ >= data_.size())
    {
        return Error{ErrorCode::OutOfRange, "Peek past end of stream"};
    }
    return data_[pos_];
}

void BinaryReader::skip(std::size_t bytes)
{
    if (pos_ + bytes > data_.size())
    {
        pos_ = data_.size();
        error_ = true;
        return;
    }
    pos_ += bytes;
}

void BinaryReader::reset()
{
    pos_ = 0;
    error_ = false;
}

}  // namespace atlas
