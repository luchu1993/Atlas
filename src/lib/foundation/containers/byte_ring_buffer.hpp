#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>

namespace atlas
{

class ByteRingBuffer
{
public:
    static constexpr std::size_t kDefaultCapacity = 65536;

    explicit ByteRingBuffer(std::size_t capacity = kDefaultCapacity)
        : capacity_(std::bit_ceil(capacity)),
          mask_(capacity_ - 1),
          buffer_(std::make_unique<std::byte[]>(capacity_))
    {
    }

    [[nodiscard]] auto writable_span() -> std::span<std::byte>
    {
        auto wi = write_pos_ & mask_;
        auto ri = read_pos_ & mask_;
        auto avail = writable_size();
        if (avail == 0)
            return {};
        // Contiguous space from write index to either end-of-buffer or read index
        auto contig = (wi >= ri) ? (capacity_ - wi) : (ri - wi);
        return {buffer_.get() + wi, std::min(avail, contig)};
    }

    void commit(std::size_t n)
    {
        assert(n <= writable_size());
        write_pos_ += n;
    }

    [[nodiscard]] auto readable_span() const -> std::span<const std::byte>
    {
        auto ri = read_pos_ & mask_;
        auto wi = write_pos_ & mask_;
        auto avail = readable_size();
        if (avail == 0)
            return {};
        auto contig = (ri <= wi) ? (wi - ri) : (capacity_ - ri);
        return {buffer_.get() + ri, std::min(avail, contig)};
    }

    void consume(std::size_t n)
    {
        assert(n <= readable_size());
        read_pos_ += n;
    }

    [[nodiscard]] auto readable_size() const -> std::size_t { return write_pos_ - read_pos_; }

    [[nodiscard]] auto writable_size() const -> std::size_t { return capacity_ - readable_size(); }

    [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }

    void clear()
    {
        read_pos_ = 0;
        write_pos_ = 0;
    }

    auto append(std::span<const std::byte> data) -> bool
    {
        if (data.size() > writable_size())
            return false;

        auto wi = write_pos_ & mask_;
        auto first_chunk = std::min(data.size(), capacity_ - wi);
        std::memcpy(buffer_.get() + wi, data.data(), first_chunk);
        if (first_chunk < data.size())
            std::memcpy(buffer_.get(), data.data() + first_chunk, data.size() - first_chunk);

        write_pos_ += data.size();
        return true;
    }

    void linearize()
    {
        auto size = readable_size();
        if (size == 0)
        {
            clear();
            return;
        }

        auto ri = read_pos_ & mask_;
        if (ri == 0)
            return;

        // Data doesn't wrap — simple shift to front
        if (ri + size <= capacity_)
        {
            std::memmove(buffer_.get(), buffer_.get() + ri, size);
        }
        else
        {
            auto temp = std::make_unique<std::byte[]>(size);
            auto first_chunk = capacity_ - ri;
            std::memcpy(temp.get(), buffer_.get() + ri, first_chunk);
            std::memcpy(temp.get() + first_chunk, buffer_.get(), size - first_chunk);
            std::memcpy(buffer_.get(), temp.get(), size);
        }

        read_pos_ = 0;
        write_pos_ = size;
    }

private:
    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<std::byte[]> buffer_;
    // Absolute positions — use & mask_ to get buffer index
    std::size_t read_pos_{0};
    std::size_t write_pos_{0};
};

}  // namespace atlas
