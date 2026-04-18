#include "serialization/binary_stream.h"

#include <cassert>

namespace atlas {

// ============================================================================
// BinaryWriter
// ============================================================================

BinaryWriter::BinaryWriter(std::size_t reserve_bytes) {
  buffer_.reserve(reserve_bytes);
}

void BinaryWriter::WriteBytes(std::span<const std::byte> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void BinaryWriter::WriteBytes(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::byte*>(data);
  buffer_.insert(buffer_.end(), bytes, bytes + size);
}

void BinaryWriter::WriteString(std::string_view str) {
  WritePackedInt(static_cast<uint32_t>(str.size()));
  WriteBytes(str.data(), str.size());
}

void BinaryWriter::WritePackedInt(uint32_t value) {
  if (value < 0xFE) {
    Write(static_cast<uint8_t>(value));
  } else if (value <= 0xFFFF) {
    Write(static_cast<uint8_t>(0xFE));
    Write(static_cast<uint16_t>(value));
  } else {
    Write(static_cast<uint8_t>(0xFF));
    Write(value);
  }
}

auto BinaryWriter::Data() const -> std::span<const std::byte> {
  return {buffer_.data(), buffer_.size()};
}

auto BinaryWriter::Size() const -> std::size_t {
  return buffer_.size();
}

auto BinaryWriter::Reserve(std::size_t bytes) -> std::byte* {
  auto old_size = buffer_.size();
  buffer_.resize(old_size + bytes);
  return buffer_.data() + old_size;
}

void BinaryWriter::Truncate(std::size_t new_size) {
  assert(new_size <= buffer_.size());
  buffer_.resize(new_size);
}

void BinaryWriter::Attach(std::vector<std::byte> buf) {
  buffer_ = std::move(buf);
}

void BinaryWriter::Clear() {
  buffer_.clear();
}

auto BinaryWriter::Detach() -> std::vector<std::byte> {
  return std::move(buffer_);
}

// ============================================================================
// BinaryReader
// ============================================================================

BinaryReader::BinaryReader(std::span<const std::byte> data) : data_(data) {}

auto BinaryReader::ReadBytes(std::size_t count) -> Result<std::span<const std::byte>> {
  if (pos_ + count > data_.size()) {
    error_ = true;
    return Error{ErrorCode::kOutOfRange, "Read past end of stream"};
  }
  auto result = data_.subspan(pos_, count);
  pos_ += count;
  return result;
}

auto BinaryReader::ReadString() -> Result<std::string> {
  auto length_result = ReadPackedInt();
  if (!length_result) {
    return length_result.Error();
  }

  auto length = length_result.Value();
  auto bytes_result = ReadBytes(length);
  if (!bytes_result) {
    return bytes_result.Error();
  }

  auto bytes = bytes_result.Value();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto BinaryReader::ReadStringView() -> Result<std::string_view> {
  auto length_result = ReadPackedInt();
  if (!length_result) {
    return length_result.Error();
  }

  auto length = length_result.Value();
  auto bytes_result = ReadBytes(length);
  if (!bytes_result) {
    return bytes_result.Error();
  }

  auto bytes = bytes_result.Value();
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto BinaryReader::ReadPackedInt() -> Result<uint32_t> {
  auto tag_result = Read<uint8_t>();
  if (!tag_result) {
    return tag_result.Error();
  }

  auto tag = tag_result.Value();
  if (tag < 0xFE) {
    return static_cast<uint32_t>(tag);
  }

  if (tag == 0xFE) {
    auto v16 = Read<uint16_t>();
    if (!v16) {
      return v16.Error();
    }
    return static_cast<uint32_t>(*v16);
  }

  return Read<uint32_t>();
}

auto BinaryReader::Remaining() const -> std::size_t {
  return data_.size() - pos_;
}

auto BinaryReader::Position() const -> std::size_t {
  return pos_;
}

auto BinaryReader::Peek() const -> Result<std::byte> {
  if (pos_ >= data_.size()) {
    return Error{ErrorCode::kOutOfRange, "Peek past end of stream"};
  }
  return data_[pos_];
}

void BinaryReader::Skip(std::size_t bytes) {
  if (pos_ + bytes > data_.size()) {
    pos_ = data_.size();
    error_ = true;
    return;
  }
  pos_ += bytes;
}

void BinaryReader::Reset() {
  pos_ = 0;
  error_ = false;
}

}  // namespace atlas
