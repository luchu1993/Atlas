#pragma once

#include <cstdint>

namespace atlas
{

using SeqNum = uint32_t;
inline constexpr SeqNum kSeqNull = 0;

// Wrapping comparison using signed arithmetic trick.
// Works correctly across uint32 boundary (e.g., UINT32_MAX vs 1).
[[nodiscard]] inline constexpr auto seq_less_than(SeqNum a, SeqNum b) -> bool
{
    return static_cast<int32_t>(a - b) < 0;
}

[[nodiscard]] inline constexpr auto seq_greater_than(SeqNum a, SeqNum b) -> bool
{
    return static_cast<int32_t>(a - b) > 0;
}

[[nodiscard]] inline constexpr auto seq_diff(SeqNum a, SeqNum b) -> int32_t
{
    return static_cast<int32_t>(a - b);
}

// ACK bitmask utilities.
// ack_num = highest received seq.
// Bit N of ack_bits means "received packet ack_num - 1 - N".

inline constexpr void ack_bits_set(uint32_t& bits, SeqNum ack_num, SeqNum seq)
{
    auto diff = static_cast<int32_t>(ack_num - seq);
    if (diff >= 1 && diff <= 32)
    {
        bits |= (1u << (diff - 1));
    }
}

[[nodiscard]] inline constexpr auto ack_bits_test(uint32_t bits, SeqNum ack_num, SeqNum seq) -> bool
{
    auto diff = static_cast<int32_t>(ack_num - seq);
    if (diff == 0)
        return true;  // ack_num itself is always ACK'd
    if (diff >= 1 && diff <= 32)
    {
        return (bits & (1u << (diff - 1))) != 0;
    }
    return false;
}

}  // namespace atlas
