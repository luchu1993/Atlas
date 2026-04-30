#ifndef ATLAS_LIB_FOUNDATION_CONTAINERS_BYTE_RING_BUFFER_H_
#define ATLAS_LIB_FOUNDATION_CONTAINERS_BYTE_RING_BUFFER_H_

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>

namespace atlas {

class ByteRingBuffer {
 public:
  static constexpr std::size_t kDefaultCapacity = 65536;

  explicit ByteRingBuffer(std::size_t capacity = kDefaultCapacity)
      : ByteRingBuffer(capacity, capacity) {}

  ByteRingBuffer(std::size_t initial_capacity, std::size_t max_capacity)
      : min_capacity_(NormalizeCapacity(initial_capacity)),
        capacity_(min_capacity_),
        max_capacity_(std::max(min_capacity_, NormalizeCapacity(max_capacity))),
        mask_(capacity_ - 1),
        buffer_(std::make_unique<std::byte[]>(capacity_)) {}

  [[nodiscard]] auto WritableSpan() -> std::span<std::byte> {
    auto wi = write_pos_ & mask_;
    auto ri = read_pos_ & mask_;
    auto avail = WritableSize();
    if (avail == 0) return {};
    // Contiguous space from write index to either end-of-buffer or read index
    auto contig = (wi >= ri) ? (capacity_ - wi) : (ri - wi);
    return {buffer_.get() + wi, std::min(avail, contig)};
  }

  void Commit(std::size_t n) {
    assert(n <= WritableSize());
    write_pos_ += n;
  }

  [[nodiscard]] auto ReadableSpan() const -> std::span<const std::byte> {
    auto ri = read_pos_ & mask_;
    auto wi = write_pos_ & mask_;
    auto avail = ReadableSize();
    if (avail == 0) return {};
    auto contig = (ri <= wi) ? (wi - ri) : (capacity_ - ri);
    return {buffer_.get() + ri, std::min(avail, contig)};
  }

  void Consume(std::size_t n) {
    assert(n <= ReadableSize());
    read_pos_ += n;
    if (read_pos_ == write_pos_) {
      Clear();
    }
  }

  [[nodiscard]] auto ReadableSize() const -> std::size_t { return write_pos_ - read_pos_; }

  [[nodiscard]] auto WritableSize() const -> std::size_t { return capacity_ - ReadableSize(); }

  [[nodiscard]] auto MinCapacity() const -> std::size_t { return min_capacity_; }
  [[nodiscard]] auto Capacity() const -> std::size_t { return capacity_; }
  [[nodiscard]] auto MaxCapacity() const -> std::size_t { return max_capacity_; }

  void Clear() {
    read_pos_ = 0;
    write_pos_ = 0;
  }

  auto Append(std::span<const std::byte> data) -> bool {
    if (data.size() > WritableSize()) return false;

    auto wi = write_pos_ & mask_;
    auto first_chunk = std::min(data.size(), capacity_ - wi);
    std::memcpy(buffer_.get() + wi, data.data(), first_chunk);
    if (first_chunk < data.size())
      std::memcpy(buffer_.get(), data.data() + first_chunk, data.size() - first_chunk);

    write_pos_ += data.size();
    return true;
  }

  [[nodiscard]] auto PeekFront(std::span<std::byte> out) const -> bool {
    if (out.size() > ReadableSize()) return false;

    auto rspan = ReadableSpan();
    auto first_chunk = std::min(out.size(), rspan.size());
    std::memcpy(out.data(), rspan.data(), first_chunk);
    if (first_chunk < out.size()) {
      std::memcpy(out.data() + first_chunk, buffer_.get(), out.size() - first_chunk);
    }
    return true;
  }

  [[nodiscard]] auto Reserve(std::size_t min_capacity) -> bool {
    if (min_capacity <= capacity_) return true;
    if (min_capacity > max_capacity_) return false;

    auto new_capacity = NormalizeCapacity(min_capacity);
    if (new_capacity > max_capacity_) return false;

    ResizeBuffer(new_capacity);
    return true;
  }

  [[nodiscard]] auto EnsureWritable(std::size_t writable_bytes) -> bool {
    if (writable_bytes <= WritableSize()) return true;
    return Reserve(ReadableSize() + writable_bytes);
  }

  void Linearize() {
    auto size = ReadableSize();
    if (size == 0) {
      Clear();
      return;
    }

    auto ri = read_pos_ & mask_;
    if (ri == 0) return;

    if (ri + size <= capacity_) {
      std::memmove(buffer_.get(), buffer_.get() + ri, size);
    } else {
      auto temp = std::make_unique<std::byte[]>(size);
      auto first_chunk = capacity_ - ri;
      std::memcpy(temp.get(), buffer_.get() + ri, first_chunk);
      std::memcpy(temp.get() + first_chunk, buffer_.get(), size - first_chunk);
      std::memcpy(buffer_.get(), temp.get(), size);
    }

    read_pos_ = 0;
    write_pos_ = size;
  }

  void ShrinkToFit(std::size_t target_capacity = 0) {
    auto desired_capacity = target_capacity == 0 ? min_capacity_ : target_capacity;
    desired_capacity = std::max(desired_capacity, ReadableSize());

    auto normalized = NormalizeCapacity(desired_capacity);
    normalized = std::max(normalized, min_capacity_);

    if (normalized >= capacity_) return;

    ResizeBuffer(normalized);
  }

 private:
  [[nodiscard]] static auto NormalizeCapacity(std::size_t capacity) -> std::size_t {
    return std::bit_ceil(std::max<std::size_t>(capacity, 1));
  }

  void ResizeBuffer(std::size_t new_capacity) {
    assert(new_capacity >= ReadableSize());

    auto new_buffer = std::make_unique<std::byte[]>(new_capacity);
    auto size = ReadableSize();

    if (size > 0) {
      auto rspan = ReadableSpan();
      auto first_chunk = std::min(size, rspan.size());
      std::memcpy(new_buffer.get(), rspan.data(), first_chunk);
      if (first_chunk < size) {
        std::memcpy(new_buffer.get() + first_chunk, buffer_.get(), size - first_chunk);
      }
    }

    buffer_ = std::move(new_buffer);
    capacity_ = new_capacity;
    mask_ = capacity_ - 1;
    read_pos_ = 0;
    write_pos_ = size;
  }

  std::size_t min_capacity_;
  std::size_t capacity_;
  std::size_t max_capacity_;
  std::size_t mask_;
  std::unique_ptr<std::byte[]> buffer_;
  // Absolute positions; use & mask_ to get buffer index.
  std::size_t read_pos_{0};
  std::size_t write_pos_{0};
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_CONTAINERS_BYTE_RING_BUFFER_H_
