#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace atlas
{

class StreamBufferPool;

class StreamBuffer
{
public:
    StreamBuffer() = default;
    ~StreamBuffer();

    StreamBuffer(const StreamBuffer&) = delete;
    auto operator=(const StreamBuffer&) -> StreamBuffer& = delete;

    StreamBuffer(StreamBuffer&& other) noexcept
        : data_(other.data_), capacity_(other.capacity_), size_(other.size_), tier_(other.tier_)
    {
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        other.tier_ = 0;
    }

    auto operator=(StreamBuffer&& other) noexcept -> StreamBuffer&
    {
        if (this != &other)
        {
            release();
            data_ = other.data_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            tier_ = other.tier_;
            other.data_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
            other.tier_ = 0;
        }
        return *this;
    }

    [[nodiscard]] auto data() -> std::byte* { return data_; }
    [[nodiscard]] auto data() const -> const std::byte* { return data_; }
    [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }
    [[nodiscard]] auto size() const -> std::size_t { return size_; }

    void set_size(std::size_t n)
    {
        assert(n <= capacity_);
        size_ = n;
    }

    [[nodiscard]] auto span() const -> std::span<const std::byte> { return {data_, size_}; }

    void clear() { size_ = 0; }

    explicit operator bool() const { return data_ != nullptr; }

private:
    friend class StreamBufferPool;

    StreamBuffer(std::byte* data, std::size_t capacity, std::size_t tier)
        : data_(data), capacity_(capacity), size_(0), tier_(tier)
    {
    }

    void release();

    std::byte* data_{nullptr};
    std::size_t capacity_{0};
    std::size_t size_{0};
    std::size_t tier_{0};
};

class StreamBufferPool
{
public:
    static constexpr std::size_t kNumTiers = 6;
    static constexpr std::array<std::size_t, kNumTiers> kSlabSizes = {256,  512,   1024,
                                                                      4096, 16384, 65536};
    static constexpr std::size_t kMaxFreePerTier = 64;

    static auto instance() -> StreamBufferPool&
    {
        thread_local StreamBufferPool pool;
        return pool;
    }

    [[nodiscard]] auto acquire(std::size_t min_size) -> StreamBuffer
    {
        auto tier = tier_for_size(min_size);
        if (tier >= kNumTiers)
        {
            auto buf = std::make_unique<std::byte[]>(min_size);
            auto* ptr = buf.get();
            // Oversized buffers go to the largest tier for return-path bookkeeping,
            // but will be discarded on release since capacity won't match any slab size.
            oversize_buffers_.push_back(std::move(buf));
            return StreamBuffer(ptr, min_size, kNumTiers);
        }

        auto& freelist = freelists_[tier];
        if (!freelist.empty())
        {
            auto buf = std::move(freelist.back());
            freelist.pop_back();
            return StreamBuffer(buf.release(), kSlabSizes[tier], tier);
        }

        auto buf = std::make_unique<std::byte[]>(kSlabSizes[tier]);
        auto* ptr = buf.release();
        return StreamBuffer(ptr, kSlabSizes[tier], tier);
    }

    void release(std::byte* data, std::size_t tier)
    {
        if (!data)
            return;

        if (tier >= kNumTiers)
        {
            // Oversized — just free it
            auto it = std::find_if(oversize_buffers_.begin(), oversize_buffers_.end(),
                                   [data](const auto& p) { return p.get() == data; });
            if (it != oversize_buffers_.end())
                oversize_buffers_.erase(it);
            else
                delete[] data;
            return;
        }

        auto& freelist = freelists_[tier];
        if (freelist.size() < kMaxFreePerTier)
        {
            freelist.push_back(std::unique_ptr<std::byte[]>(data));
        }
        else
        {
            delete[] data;
        }
    }

    StreamBufferPool(const StreamBufferPool&) = delete;
    auto operator=(const StreamBufferPool&) -> StreamBufferPool& = delete;

private:
    StreamBufferPool() = default;

    static auto tier_for_size(std::size_t size) -> std::size_t
    {
        for (std::size_t i = 0; i < kNumTiers; ++i)
        {
            if (kSlabSizes[i] >= size)
                return i;
        }
        return kNumTiers;
    }

    std::array<std::vector<std::unique_ptr<std::byte[]>>, kNumTiers> freelists_;
    std::vector<std::unique_ptr<std::byte[]>> oversize_buffers_;
};

inline StreamBuffer::~StreamBuffer()
{
    release();
}

inline void StreamBuffer::release()
{
    if (data_)
    {
        StreamBufferPool::instance().release(data_, tier_);
        data_ = nullptr;
        capacity_ = 0;
        size_ = 0;
    }
}

}  // namespace atlas
