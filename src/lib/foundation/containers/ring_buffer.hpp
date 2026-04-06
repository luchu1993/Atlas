#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

namespace atlas
{

template <typename T>
class RingBuffer
{
public:
    explicit RingBuffer(std::size_t capacity)
        : buffer_(capacity), capacity_(capacity) {}

    auto push_back(const T& item) -> bool
    {
        if (full()) return false;
        buffer_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
        return true;
    }

    auto push_back(T&& item) -> bool
    {
        if (full()) return false;
        buffer_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
        return true;
    }

    auto pop_front() -> std::optional<T>
    {
        if (empty()) return std::nullopt;
        T value = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        return value;
    }

    [[nodiscard]] auto front() const -> const T&
    {
        assert(!empty());
        return buffer_[head_];
    }

    [[nodiscard]] auto back() const -> const T&
    {
        assert(!empty());
        return buffer_[(tail_ + capacity_ - 1) % capacity_];
    }

    [[nodiscard]] auto size() const -> std::size_t { return size_; }
    [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }
    [[nodiscard]] auto empty() const -> bool { return size_ == 0; }
    [[nodiscard]] auto full() const -> bool { return size_ == capacity_; }

    auto operator[](std::size_t index) const -> const T&
    {
        return buffer_[(head_ + index) % capacity_];
    }

    auto operator[](std::size_t index) -> T&
    {
        return buffer_[(head_ + index) % capacity_];
    }

    void clear() { head_ = 0; tail_ = 0; size_ = 0; }

private:
    std::vector<T> buffer_;
    std::size_t capacity_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
};

} // namespace atlas
